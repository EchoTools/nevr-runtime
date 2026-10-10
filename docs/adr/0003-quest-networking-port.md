# ADR 0003: Quest networking shares the PCVR protocol core and differs only in adapters

Status: accepted. The login-profile builder, the EVR frame codec and the redirect policy are
shared today (`src/runtime/compat/login_profile.{h,cpp}`, `src/runtime/compat/evr_codec.{h,cpp}`,
`src/runtime/lifecycle/service_redirect.{h,cpp}`).
The social facade is implemented and host-tested in `src/quest/social/`. The rest is not
implemented; the work is tracked in #158 and the test regime is ADR 0004.

## Context

The Quest client needs to reach the same community services as PCVR while using Android-specific
loading, game ABI, hook installation, configuration, logging, and socket integration. Duplicating
protocol behavior would make the clients diverge.

## Decision

Keep protocol and social rules in shared source and isolate platform-specific behavior in adapters.
The Quest client must use the same implementation of each nEVR protocol and social rule as PCVR.

## Consequences

The Android/arm64 Quest client reaches the community service, completes config and login,
connects to matchmaking, enters a social lobby, and supports the Windows client's friends and
party behavior. Android supplies its own loader, game ABI, hook installation, logging,
configuration discovery and socket adapter, and the original Oculus loader stays available to
the game.

Scope is the client. The dedicated server, `src/legacy/` and production server deployment are
out of scope.

## What the Quest target is today

`src/quest/` builds a crash sentinel (`sentinel/entry.cpp`, `sentinel/sentinel.cpp`) and a GOT
import hook (`sentinel/got_hook.{h,cpp}`); it does not build the Windows runtime. The Android
preset (`src/quest/CMakePresets.json`) uses the Quest-local vcpkg manifest, the `arm64-android`
triplet and the NDK chainload at API 26. `nevr_quest_login_profile` compiles the shared login
profile, and the sentinel links it through `nevr_quest_login`.

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
    was involved: the server refused a poll with a 4xx, or the bound on unanswered codes was
    reached. A code that no mechanism could show (the file not written and the game text refused)
    is not polled; one `{"mechanism":"all","result":"not_shown"}` line says so, and a new code is
    tried at the next recovery attempt.
  - A code that runs out (the server answers `expired`, or its five minutes pass) is replaced: the
    state stays `AwaitingUser`, a new code is requested (no sooner than 30 s after the previous
    request) and shown in place of the old one, and the prompt is not taken down in between.
    `Session::kMaxUnansweredCodes` (6) bounds the codes a session issues without a sign-in, counted
    across every device login of the session (renewals and recovery attempts alike; a sign-in
    resets it). The server does not rate-limit code requests, so this is the client's bound: per
    game start without a sign-in, at most six codes, at least 30 s apart. After the sixth the
    player is told "Sign-in timed out. Restart the game to try again.", or "No sign-in code could
    be shown. Restart the game to try again." when none of those codes could be shown, and the
    login is `Failed`, final. The bound is checked before a device login sets `AwaitingUser`.
  - A poll that answers `verified` is taken even when it returns after the code's deadline: the
    server deletes the code when it hands out the tokens (`evr_device_auth.go`, the verified branch
    of the poll RPC).
  - While the player holds a link, poll failures that are transient (no connection, 5xx, 408, 429)
    are waited out until the code's own five-minute deadline at the normal poll interval.
  - After login, a refresh token that has expired or that the server refuses publishes `Expired`,
    logs once and starts the device login again; other refresh failures keep the login and retry
    next period.

`nevr_quest_token_auth` is linked into the sentinel; `integration/production_steps.cpp` starts the
session and `integration/identity_source.cpp` hands its token to the login rewrite. The tests are `src/quest/tests/auth_core_test.cpp` (fake HTTP and clock) and
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
| `file` | `device_login.txt` (URL, code, instructions, expiry) under the external files dir | `written` / `write_failed` (with the page URL, not the code) |
| `game_error_text` | the prompt as the game's own login-error screen | `published` (`what`: `code`, `signed_in`, `timed_out`, `no_code_shown`) / `refused` / `withdrawn` |

There is no Android intent, toast or notification: the sentinel holds no `JavaVM` or activity
object (it is loaded as a `DT_NEEDED` dependency, so its `JNI_OnLoad` is not called).

What the game does, measured on the pinned `libr15.so` and `libpnsovr.so`:

- A login that fails before it is sent (`CNSOVRUser::UpdateInternal`, `libpnsovr.so` `0x1edb64`
  to `0x1edb7c`: code 500, "Log in request failed: One or more prerequisites are missing") calls
  the vtable slot at `0x6a12a0`, `CNSOVRUser::LogInFailed` (`0x1ec5d0`), which tail-calls
  `CNSUser::LogInFailed`; that logs `[LOGIN] %s` and calls the login-failed delegate. A server
  `SNSLogInFailure` reaches the same `CNSUser::LogInFailed` through `LogInFailureCB`
  (`libr15.so` `0x1933928`). The local texts ("Log in request failed: " plus "One or more
  prerequisites are missing", "Failed to get user proof", "Client error", "Cryptography error",
  "Service unavailable") are referenced only on those login paths.
- `CR15NetGame::LogInFailedCB` (`libr15.so` `0x125f298`, whose address `CR15NetGame::Initialize`
  loads from the `GLOB_DAT` at `0x3708518`, at `0x12861c0`) calls `SetDelimitedErrorMessage` through PLT `0xf23510`
  (`R_AARCH64_JUMP_SLOT` `0x36e9170`), then `SwitchTo(-0x5e)`. The other two callers are
  `LoginRemovedCB` (`0x125f9b4`, which acts only when the state is 3 or higher, logged in) and
  `LocalUserProfileErrorCB` (`0x126d588`, only in state 2, logging in).
- `SetDelimitedErrorMessage` (`0x125f768`) splits the message on `'\n'` into at most four lines and
  calls `SetErrorMessage`, which writes the error block at `CR15NetGame+0x63308` (a byte that is 0
  for one line and 1 for two or four, then four 64-byte lines), logs `[NETGAME] %s %s %s %s`, and
  builds a JSON record of the lines in a stack-allocated temporary string that it finishes through
  the temporary's own vtable (the indirect call at `0x12413f4`/`0x1241858`) and frees; the record is
  not passed to a logger or anything else. `CR15NetErrorMessageExpression`
  (`0x23225d0`) copies the block to the UI script.
- The state is the `int` at offset 0 (`SwitchTo`, `0x125b8b4`); `GameStateString` (`0x124d478`)
  names 2 "logging in", 3 "logged in", -94 "login failed", 0 "logged out". Entering -94 runs
  `ScheduleQuitOnError`, whose deferred `QuitOnError` (`0x12713f8`) takes one of three paths:
  - `[CR15Game+0x1f08]` set (`0x1271408`, `cbz` at `0x1271410`): it sets bit `0x200000` in the
    flags word that the pointer at `CR15NetGame+0x2da0` points to (`0x1271404` loads the pointer,
    `0x127140c` the word, `0x1271414`/`0x1271418` set the bit and store it) and returns. No
    multiplayer is ended, no event sent, no quit requested. `CR15NetGame::Update` schedules it
    again when bit 14 of that word is clear (`tbnz` at `0x1294c40`), its argument is 1
    (`0x1294c44`/`0x1294c48`), bit `0x200000` is set and `[CR15Game+0x1f08]` is zero
    (`0x1294c4c`-`0x1294c60`).
  - otherwise it ends multiplayer (`CR15Game::EndMultiplayer`, `0x127144c`), then with
    `[CR15Game+0x7af0]` set sends that game space a component event;
  - or, with it zero, stores 1 at `[*0x376ce08 + 0x16db8]`, the flag `CR15Game::UpdateGame` sets
    when `CVR::ShouldQuit()` returns true (`0x11fb598`-`0x11fb5b4`), that is, a quit request.

  Which path runs on a headset is not measured. A new
  login is started by the UI script (`CR15NetBeginLoginNode::Enter`, `0x231c118`, calls
  `BeginLogIn`), not by `CR15NetGame`. The only `SwitchTo(0)` is in `CR15NetGame::LogOut`
  (`0x1289168`), whose only caller is `~CR15NetGame` (`0x128824c`), whose only caller is
  `CR15Game::ShutdownEngine` (`0x11f4fe4`): a "login failed -> logged out" line is the engine
  shutting down.
- The login-failed page's status script (`lib674f3b71da94770c.so`) copies the error block
  (`CR15NetErrorMessageExpression`, `0x23225d0`) into its four text elements only on the game's error
  event (`delegate_onnetgameerror`, sent from `CR15NetGame::QuitOnError` `0x12713f8`); a block
  rewritten afterwards is not on the screen until that event is sent again.
- So the prompt must also work if the game quits after the failure: the player starts the game
  again. A code the player had not used is then gone (nothing polls it any more) and the new start
  shows a new one, in the game text and in `device_login.txt`; a sign-in that finished before the
  quit is in the credential cache and logs in. The signed-in notice says "Select RETRY to
  finish.": the new sign-in goes to the credential cache and `QuestTokenAuth::Token()`, nothing hands
  it to a login the game is already running, and the login-failed screen's RETRY button starts a new
  login, which asks for the stored sign-in. The player-facing steps are in
  `docs/quest/SIGN-IN.md`.

How the prompt gets there (`auth/prompt_board.h`, `sentinel/login_prompt_hook.h`):

- Token auth writes the text to the prompt board, a sequence-locked fixed buffer built without
  exceptions, in one of two modes: `prompt` (the code; after the last code, "Sign-in timed out.
  Restart the game to try again.", or "No sign-in code could be shown. Restart the game to try
  again." when none of the codes could be shown) or `notice` ("Signed in to EchoVRCE. Select RETRY
  to finish.", after a sign-in). A notice replaces a prompt or a notice the screen shows, and is shown for a
  local login failure that arrives while it is on the board (the player signed in while that attempt was
  in flight); on a screen that showed the game's own text and no prompt it is not applied later.
  Publishing rewrites the whole buffer and withdrawing zeroes it.
- A GOT hook on `SetDelimitedErrorMessage` lets the game store and log its own message first, so
  the code never passes through the game's logging. If the game was logging in (state 2), the
  message is exactly one of the local texts above (`src/quest/game_login_failures.h`, which
  `got_pinned_test.cpp` checks against the pinned `libpnsovr.so`; a server-sent message, such as a
  ban or a suspension, is never replaced) and the block holds that message, the instance is
  followed from then on and the game's block saved; if the board holds a `prompt` it is written
  over the block at once.
- A GOT hook on `CR15NetGame::Update` (`0x1294b40`, `R_AARCH64_JUMP_SLOT` `0x36e05f8`, called from
  `CR15Game::UpdateGame` at `0x11fb5cc`, up to four times per game-loop iteration, see below)
  rewrites the followed block while
  that instance is still in -94 and the board or the block has changed: a prompt published after
  the failure, a new code, the signed-in notice (only over a prompt), the timed-out text, or the
  game's saved block when the board is withdrawn. On every call it checks the block still holds
  what the hook last left there. The game writes the same block for other errors (for example
  `CR15NetClientLobby::SwitchTo` from `0x123e950`, `LobbySessionFailureCB` `0x1240fa4`/
  `0x1240fcc`, `LobbyStatusNotifyCB` `0x125fa88`/`0x125fa9c`/`0x125fb58`, `CR15NetGame::SwitchTo`
  `0x125bba4`/`0x125bbe8`/`0x125bc44`, `OnGameSpaceUnloaded`/`Aborted` `0x12939f4`/`0x1293ad4`);
  a block another writer changed is left alone for good, unless it holds one of the local
  login-failure texts again, which is taken up as the game's text and gets the prompt: a new local
  failure the error hook could not take up (its writer flag was held), or one the error hook did
  not take up because the game was not logging in at that moment (already in -94; that call also
  counts `_text_not_local`).
- After a rewrite while the instance is in -94 the Update hook calls `CR15NetGame::QuitOnError` once
  so the status script copies the new text to the screen: at most once per change, not within 50 ms of
  the previous call or the failure's own error event (longer than a loop iteration, so never twice in one frame), only for the followed
  instance and only while it is still in -94, from the game thread. `QuitOnError` is not a GOT
  target; the install proves it (the module's build ID, then its first four instructions, then
  `base + 0x12713f8`) and logs `quit_on_error` in `login_prompt_install`. The extra call also ends
  multiplayer once more (`Ending multiplayer`) and clears flag bits at `+0x2da0`; it switches no state.
- The prompt latch and a GOT hook on `CR15UIPage2EnablePageNode::Enter` (`0x1fc210c`,
  `R_AARCH64_JUMP_SLOT` `0x36c1cf8`; `x0` is the node, `x1` the record, `node == record + 0x20`; the record's
  `+0x10` is the target page's level actor id; the one caller, `BindBranchingNode` `0x2005aa4`, ignores the
  return and the node writes nothing to its script thread). After the player selects RETRY, the
  login-failed page's script starts a login and enables the logging-in page (`logging_in_page`,
  `0xee753e35461e0ef4`); that wakes a script parked since boot (`libfe3f05ac05841a2e.so`, on
  `delegate_onnetgameerror`) which enables the error page (`error_display_page`, `0x4b8a0630361f3ac5`;
  `fatal_error_display_page`, `0xe26415a8c369eb2e`), and the logging-in page has no header and no buttons.
  The hook skips (a plain return) the enable of the error pages while the latch is armed, and the enable of
  the logging-in page while the latch is armed and the login may not proceed. "May proceed" is one
  process-wide word (`quest/login/login_attempt_gate.h`) that the token-auth source publishes and that
  both this hook and the login prerequisites (`IdentitySource::Ready`) read, so they cannot disagree: the
  skip stops in the same instant the login may proceed, and an attempt whose logging-in page was skipped is
  poisoned and fails its prerequisites until the game leaves "logging in", even if the gate turns ready
  meanwhile (otherwise the login would succeed on a screen that cannot show it). The signed-in notice is
  applied to the block only once the login may proceed, so "Select RETRY" never appears while that RETRY
  would be held back. The latch is armed when the Update or error text
  hook writes a prompt or a notice into the error block, cleared as soon as the block holds anything else
  (checked after every write the error text hook sees and on every Update call), and dead for good when
  the game reaches "loading global" (state 4). It does not depend on the followed instance, which is dropped
  when the game leaves "login failed". The latch flag is recomputed only on the game thread, so the hook also
  hashes the live block against the latched hash (lock-free, bracketed by a sequence counter around the
  sentinel's own writes) before it skips anything: a genuine error the game's unhooked `SetErrorMessage`
  wrote in the same frame is never dropped. The hook may run on a task-scheduler worker thread
  (`CScriptCS::UpdateScripts` may use `CComponentSystem::TaskedUpdate`), so it reads atomics only; the
  records at other pages go to the game unchanged.
- The Update and error text hooks run on one thread: each `CncaGame::RunLoop` iteration calls `CR15Game::Update` (vtable
  slot `0x408`) four times, with the arguments 0 to 3 (`0x17ec5ec`, `0x17ec5fc`, `0x17ec610`,
  `0x17ec624`). Only the call with argument 0 updates the login providers (`cbz x1` at
  `0x11fd838`; `CNSProvider::Update` at `0x11fd858`-`0x11fd870` and
  `CR15NetGame::UpdateBroadcaster` at `0x11fd878`, where login failures are delivered); the calls
  reach `CncaGame::Update` (`0x11fd8ac`), which calls `CR15Game::UpdateGame` (slot `0x198`), which
  passes its argument on to `CR15NetGame::Update` (`0x11fb134` keeps it in `x19`, `0x11fb5c8`/
  `0x11fb5cc`). So the Update hook runs up to four times per loop iteration. That the login-failure callbacks run inside those provider and
  broadcaster updates is inferred from the call chain, not traced instruction by instruction; the
  hooks' writer flag does not rely on it.
- No hook logs or takes a lock. Their fourteen counters are `login_prompt_text_shown`,
  `_text_refreshed`, `_text_kept`, `_text_not_local`, `_board_busy`, `_block_not_ours`, the three
  thunks' fault counters, `_error_page_dropped`, `_logging_in_page_dropped`, `_page_passed_armed`,
  `_error_resent` and `_error_resend_unavailable`; installing logs one `login_prompt_install` line (or `skipped` when the
  counters were refused).

Exposure of the device code. It is shown to the player by design and is written to
`device_login.txt` on `/sdcard`, readable by apps with storage access, until the login ends (or,
after a crash, until the next start). No client log line carries it (the sentinel's, token auth's
and, because of the hook order above, the game's own); the server logs it at Info when it is
verified (`evr_device_auth.go`, `DeviceAuthVerifyRpc`). A crash while a code is live can put it in
a minidump: the sentinel's Breakpad handler writes dumps to the first writable of
`/sdcard/Android/data/com.readyatdawn.r15/files/nevr-crashes`, the app-internal `files`
directory and `/data/local/tmp` (`sentinel/sentinel.cpp`), and a dump holds process memory: the
game's error block while it shows the code, library buffers (libcurl's request and response) and
anything else. The client overwrites these copies when it is done with them: the board (on
withdraw and on each publish), the hooks' stack copies and their record of the block, the flow's
code and URL, the session's prompt, and the presenters' formatted texts. Temporaries made while
building those strings, and copies held by libraries and the game, are not reached. While a code lives (single use,
five minutes) it is also the poll credential: whoever polls it after the player verifies receives
the tokens. The verify RPC signs the code in to the account of whoever calls it
(`DeviceAuthVerifyRpc` uses the caller's user id), so someone who reads the code can make this
headset sign in to their own account, not take over the player's. Those limits are why the code is
shown on screen and kept in that file, and why it is kept out of the client's logs.

Nothing here holds the game's login while the player signs in; that belongs to whatever intercepts
the game's login request.

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
   `src/` and fails if a file other than `src/quest/sentinel/got_hook.h` and the files under
   `src/quest/tests` names the test access class, or if a file outside a `tests/` directory includes
   (`#include` or `#include_next`, resolved relative to the including file, to `src/` and to
   `src/quest/sentinel`) anything under `src/quest/tests`, recursively. A symlink under `src/` fails
   the test, because the walk does not follow one.
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
only if it changed. The counter table holds 96 counters for the whole program (`sentinel::kMaxReportCounters`), and every
`RegisterReportCounter` call must come before `StartReporter` (a later registration, or the 97th, is
refused and logged as `register_refused` with its `reason`: `reporter_running`, `table_full` or
`null_argument`). The constructor therefore registers every counter first and starts the reporter once;
`StopReporter` forgets the table, so a Stop, Register, Start sequence loses the counters registered before
the Stop. The thread ends with the process; creating it from a constructor on a Quest is
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
`clock_gettime`, `CR15NetGame::SetDelimitedErrorMessage`, `CR15NetGame::Update` and
`CR15UIPage2EnablePageNode::Enter` (installed by `entry.cpp`),
`CJson::TString` in both libraries, and the `SNSConfigRequestv24Send` and `GLOB_DAT` slots as
fixtures. Only `clock_gettime`, `SetDelimitedErrorMessage`, `CR15NetGame::Update` and
`CR15UIPage2EnablePageNode::Enter` are installed.
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
3. **Session routing.** `src/runtime/compat/session_router.{h,cpp}` is the platform-neutral router:
   no Windows or Winsock headers, no sockets, no threads. Game-side and remote open/frame/close
   events go in; the router owns connection identity (config, login, matchmaker; matchmakers
   attached to the login session, at most `Limits::maxMatchmakerConnections` live), frame order,
   size limits, bounded queues and
   backpressure, and, when a remote session ends, the close of every game socket on it plus
   forgetting the session so the next connection is a new login. The game transport, the remote
   transport, the login-frame builder and the log sink are injected; the router calls none of
   them under its lock. No token, password, full login frame or credential URL is logged.

   **A connection's role is named by its first data frame** (`ClassifyFirstFrame`): the config
   connection always opens with `SNSConfigRequestv2`; a matchmaker connection with one of
   `SNSLobbyMatchmakerStatusRequest`, `FindSessionRequestv11`, `CreateSessionRequestv9`,
   `JoinSessionRequestv7`, `DirectoryRequestJsonv2`, `PendingSessionCancelv2`,
   `PlayerSessionsRequestv5` or `PingResponse`; the login connection sends nothing until
   `LogInRequestv2`. Until that frame the role is provisional, by connection order (0 config, 1 login,
   later ones matchmaker), so the remote can open before the game has spoken. A first frame that
   contradicts the provisional role moves the connection to the remote its real role uses; only
   `LogInRequestv2` moves a connection to login (an unlisted message never takes the login role). The
   game opens a new config connection after login, and by order that connection is a matchmaker: the
   first frame is what makes it a config connection with a remote of its own.

   **Frames from the login session's remote are routed by role**, never to the socket that spoke
   last. libpnsovr drops a reply to a login-connection request that arrives on another peer
   (`CNSUser::ProfileSuccessCB` and the other login-peer checks), so login and profile replies,
   document and other-user-profile replies and the login settings (`IsLoginSessionReply`) go to the
   login connection, and are dropped (counted, logged) when it is gone. Lobby traffic goes to the newest
   matchmaker connection, falling back to the login connection. An `STcpConnectionUnrequireEvent`
   lowers the outstanding-request count of the connection it arrives on, and that count is 8 bits: an
   Unrequire with none outstanding wraps it to 255 without a sound, and the login connection's later loss
   then raises "connection lost" instead of being ignored. So the router keeps the game's own count per
   connection (`RequestRaisesRequireCount`: every login-connection send but `LogOut`, `TelemetryEvent` and
   `RemoteLogSetv3`; every matchmaker send but `MatchmakerStatusRequest`, the `PingResponse` included;
   `SNSConfigRequestv2` on the config connection) and never delivers an Unrequire to a connection with
   nothing outstanding; such an Unrequire is dropped and counted (`Stats::droppedUnrequires`; the token-auth poll logs
   `router_unrequire_dropped` with the totals and the increases each time one changed, because the reporter's
   counter table is nearly full).
   **A limit the router cannot remove.** An Unrequire cannot be taken out of a frame. With nakama's
   `DisableLoginMessage` branch (`evr_pipeline_login.go`: the failure and its Unrequire, then a login that
   still succeeds) the success frame's embedded Unrequire arrives after the failure's Unrequire already
   covered the login request, so it reaches the game with nothing outstanding and wraps the game's count; the
   router counts it (`Stats::unmatchedEmbeddedUnrequires`, the `unmatched_embedded_*` fields of the same
   line) and does not prevent it. The same count covers an Unrequire inside a frame the router drops. The service
   pairs an Unrequire with `LoginFailure` (every login failure is a `LoginFailure` frame and then a standalone
   Unrequire), `ChannelInfoResponse`, `DocumentSuccess`, `UpdateProfileSuccess`, config replies
   and its own `LobbyPingRequest`; it sends the Unrequire as a frame of its own from a concurrent goroutine,
   so frames interleave and the router queues, per paired message, the connection that message went to and
   gives each standalone Unrequire to the queue's front. The login reply is one frame, [`LogInSuccess`,
   Unrequire, `SNSLoginSettings`], delivered whole to the login connection; an Unrequire inside a frame lowers
   the count of the connection the frame goes to. `ChannelInfoResponse` goes to the login connection (the
   game accepts it on any peer, but its Unrequire has to land where the count is). A `LobbyPingRequest` goes
   to the matchmaker connection and is dropped, with its Unrequire, when there is none: the ping discovery
   sends one on the login session right after the login. `TestSmokeSequenceNewConfigSocketIsConfigAndProfileReplyReachesLogin` and
   `TestServerFramesRouteByRole` pin this.

   **A held login.** Before the player signs in there is no account token, and the login
   connection (silent, long-lived: the game fails over to "service unavailable" after three failed
   connects) must not fail. `Options::loginGate` answers Awaiting, Ready or Refused
   (`TokenIdentitySource::GateFor`, kept current by the token-auth poll as a lock-free atomic).
   Awaiting covers every state in which a token is still to come, including a failure the session
   retries every recovery period (`Snapshot::will_retry`) and an expired token being replaced; only a
   final failure, a stopped session or a token without an account is Refused.
   While it answers Awaiting the router creates the login session's record but does not open its
   remote (`Stats::heldRemotes`), and queues what the
   game sends in order. The login connection is silent until the game's `LogInRequestv2` (the game
   sends none before the player has signed in, however long that takes), so the router tells the
   transport (`GameTransport::SetIdleExempt`) that it is never to be closed for sending nothing, held
   or released, for as long as it is the login connection; `LoopbackGameServer` then exempts it from
   `idleFirstFrameMs` (pings are still answered). Config, matchmaker and unclassified connections
   keep the idle close. `ReevaluateHeldLogins`
   (via `SessionBridge::ReevaluateLoginGate`, called when the gate changes) opens the remote on
   Ready and closes the connection with 1011 on Refused. Config and matchmaker connections are
   never held: with no token their remote cannot start and they close with 1011 at once. A
   connection opened while the login session has no live login connection is the login connection of
   that session (held again if the account is still awaited).

   **Login injection is mutually exclusive with the game's own login.** The router injects a
   LoginRequest (and, separately, a friend-list subscribe after LoginSuccess) only when the
   wiring sets `Options::buildLogin` (and `subscribeFriendList`); both are off by default. The PC
   bridge turns them on because pnsrad sends no login. On Quest the game's own login, rewritten
   in place (PR #221), is the only login: `SessionBridge::Config` has no login builder, the
   Quest router runs with the defaults, and `TestQuestDefaultsInjectNothing` pins that it sends
   exactly the frames the game sent. Enabling both would send two logins.

   **A lost login session under a silent game (#320).** When the remote session ends and the game's login
   connection has nothing outstanding, `SConnection::DisconnectCB` (`libr15.so` `0x24fc17c`) raises no Lost
   event and the game reconnects the socket without logging in, so the new session would sit
   unauthenticated. The router does not replay a login and keeps no credential: with
   `Options::loginRemovedJson` set (Quest) it keeps the 16-byte account id of the last `LoginSuccess` and
   sends the game an `SNSLoginRemovedNotify` (`nevr_evr_codec::BuildLoginRemovedNotify`) on each
   game socket that reconnects, once per socket. The game's own handler (`CNSUser::LoginRemovedCB` `0x1933a28`,
   `CR15NetGame::LoginRemovedCB` `0x125f908`) acts only on the login peer, for the account it holds, when
   it is logged in: reason 1 shows the JSON `message` on the login-failed screen (`SwitchTo(-94)`), whose
   RETRY runs the game's own login with the current token. Nothing in that path persists anything (every
   call is listed in the PR for #320). The router cannot tell the login socket from the others (connection order and silence are guesses),
   so the notice goes to every socket that reconnects after the loss: on its first frame, or when it has
   stayed silent for `silentNotifyMs` (`Router::OnGameSilent`), once per socket; the game drops it on any
   peer but its login peer. It stays armed until the game sends its own `LoginRequest` or the next session
   answers `LoginSuccess`, and is not armed when the login connection had requests outstanding (the
   game's Lost path, -95, already shows RETRY) or its socket had already closed. The
   fixed part of the frame (0x18 bytes) is derived from the callbacks, not captured: the 4 bytes at `+0x10`
   and the id word order are the constants `kLoginRemovedWord10` and `kLoginRemovedUserIdSwapped`, the first
   things a headset run checks. The player leaves the current lobby when it arrives (`QuitOnError` ->
   `CR15Game::EndMultiplayer`, the same as any login failure from a logged-in state).

   `src/quest/net/` holds the Android adapters.
   - `loopback_game_server`: a POSIX WebSocket server bound to `127.0.0.2` (`kListenAddress`) on an
     ephemeral port (RFC 6455 in `ws_wire`). **Access control:** every local app can reach that
     port, and connection identity is arrival order, so an unauthenticated connection could
     become the login or share the login session. `Start()` therefore draws 128 random bits from
     the kernel (`getrandom`), `LoopbackUri()` returns `ws://127.0.0.2:<port>/<token>/`, and an
     upgrade is answered 403 unless its request target carries the token (first path segment, or
     the query parameter `nevr_token`), compared without an early exit, and has no `Origin`
     header. The token and the request target are never logged; the refusals are counted
     (`RejectedUpgrades()`) and logged once each with a generic reason. A connection that
     completes the upgrade but sends no data frame within `idleFirstFrameMs` is closed with 1008,
     so idle outsiders cannot hold the connection limit. The redirect hook must use `LoopbackUri()`
     as the replacement value: `nevr_cfg::ResolveRedirect` with `bridgeActive` returns the bare
     `ws://127.0.0.1:<port>`, which carries no token and which the game cannot reach at all (next
     paragraph).
   - `remote_ws`: the remote transport and its policy: `wss://` only, one connect attempt per
     session, no retry and no downgrade after a failure.
   - `curl_ws_connector`: libcurl from the Quest vcpkg manifest with peer and host verification
     always on, TLS 1.2 or later, redirects off, proxies off (`CURLOPT_NOPROXY "*"`: libcurl
     otherwise reads `all_proxy`, `https_proxy`, `wss_proxy` from the environment), and trust
     loaded from the Android CA directories into an in-memory `CURLOPT_CAINFO_BLOB` by the loader
     token auth uses. The `just verify` sensor on that file (every option an allowlisted
     `CURLOPT_` literal, each critical option set exactly once) is a tripwire, not the guarantee:
     `just test-quest-tls` runs the connector against real servers and is what proves the
     behavior.
   - `session_bridge` composes them, with the frame tap (`frame_tap`) and the side-channel send the social
     facade needs (`Config::tap`, `SendToLogin`).

   The Windows `ws_bridge.cpp` does not use the router yet; it keeps its own copy of these rules.

   **Which address the game dials (measured).** The game's peers (`SClientData::STcpPeerData::Connect`
   `0x24fd120`, `SWebSocketData::Connect` `0x2adadbc`, `SServerData::CreatePeer` `0x2505000`) resolve
   their host through `CDnsLookup::Lookup` (`0x193856c`) and `NRadEngine::CSysNet::Lookup(char const*,
   unsigned short)` (`libr15.so` `0xf991a4`; `CDnsResolver::Lookup` `0x1939250` hands numeric hosts to it
   too). `CSysNet::Lookup` compares the host with `"localhost"` and `"127.0.0.1"` (`CSysString::Compare`
   `0xf824ac`, whole string, ASCII case-insensitive). On a match, or an empty host, it calls `getifaddrs`
   and dials the first IPv4 interface that is running and not loopback (`ifa_flags & 0x48 == 0x40`), or,
   when none is running, the first IPv4 interface that is not loopback. Every other host goes to
   `getaddrinfo(host, "%hu")` and is dialled as written. So the game never dials `127.0.0.1`; on the
   Quest it dials wlan0, where nothing listens, and the connect is refused. The listener therefore binds
   `127.0.0.2`: Linux and Android route all of `127.0.0.0/8` to `lo`, so it stays loopback-only, and
   `GameDialsHostVerbatim` (`loopback_game_server.h`) with a `static_assert` keeps `kListenAddress` out
   of the substituted set. `libpnsrad.so`, `libpnsradmatchmaking.so` and `libpnsovr.so` link their own
   copies of `CSysNet::Lookup` with the same rule. `TestGameDialsTheRedirectUri` resolves
   `LoopbackUri()` by this rule and must reach the listener.

   **How the game carries the token (measured, not assumed).** The game's WebSocket client,
   `NRadEngine::CWebSocketCodec::SendHandshakeRequest` (ReVault `libr15.so` `0x2ad8928`, ghidra
   raw decompilation), formats the request as
   `GET %s HTTP/1.1\r\nHost: %s%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n`,
   with the target built as `"%s%s%s%s"` from `"/"` (only when the URI's path is empty), the URI's
   path component, `"?"` (only when the query is non-empty) and the URI's query component. The
   codec therefore sends the path and query of the URI it was given verbatim, adds no fixed path,
   and sends no `Origin` header, so a path prefix or a query parameter in the redirect value
   reaches the listener and rejecting `Origin` cannot refuse the game. Not established: how the
   caller assembles the URI it hands the codec from the config value (whether a service path is
   appended to the config string before `CUriContainer::Parse`); the callers
   (`CNSRadService`, `CR15NetGame`) were not traced. If the game replaced both path and query,
   neither form could carry the token and a per-role port would be the alternative. ReVault's
   `libr15.so` bytes match the shipped build (sha256 `8dd9a961...1d8b20`) at `0xf991a4` and
   `0x1939250`; the rest of it was not compared, and the addresses in the
   "Config-string seam" table did not resolve in ReVault under that spelling.

   **Residual risk.** A process running as the same Android uid as the game can read the token
   from the game's memory or from the redirect value, and can then connect. Processes of other
   uids cannot reach it through the network stack's loopback port without the token. The token
   protects against drive-by local apps and webviews, not against code inside the game's own
   sandbox.
4. **Game hooks.** The Android adapter records the ELF build ID or SHA-256, module and load
   bias, validates each instruction, string and relocation, then installs a typed callback.
   Callbacks use bounded copies, preserve object ownership and return semantics, never throw
   through the game ABI, and defer networking and file I/O out of loader constructors. The
   sentinel's ELF constructor (`nevr_sentinel_ctor`) is the exception, and does exactly this, in
   this order:
   - logs one `sentinel_ctor` record (logcat only);
   - `Arm()`: `mkdir`/`access` on up to four candidate crash-dump directories, then allocates the
     Breakpad exception handler (`new`), outside any `try` block. The `ExceptionHandler` constructor
     (`extern/breakpad/src/client/linux/handler/exception_handler.cc`) allocates a signal stack with
     `calloc` when the thread has none or a smaller one (`InstallAlternateStackLocked`, at least
     16 KiB), installs handlers for `SIGSEGV`, `SIGABRT`, `SIGFPE`, `SIGILL`, `SIGBUS` and `SIGTRAP`
     (`InstallHandlersLocked`, `kExceptionSignals`), and creates a dump GUID from `getrandom`
     (`GRND_NONBLOCK`) or, failing that, `/dev/urandom` (`extern/breakpad/src/common/linux/guid_creator.cc`);
   - `InitActivation()`: opens `nevr-quest.json` read-only and `nevr-sentinel.log` read-write in
     append mode in the app's external files directory. Both opens are non-blocking, the log open
     does not follow symlinks, and each target must be a regular file (a FIFO or device is
     refused, not opened for I/O). The config read is bounded at 64 KiB. A failed log open (a
     missing directory, or a directory or a symlink at the path) is retried at most once per 30 s,
     measured on `CLOCK_MONOTONIC`; a path that opens but is not a regular file (a FIFO or a device)
     is never retried. The catch block of `InitActivation()` allocates nothing, so a failure there
     cannot escape the constructor;
   - registers every counter, then starts the counter reporter thread (the reporter refuses a later
     registration), and only then installs one GOT hook on `libr15.so`'s `clock_gettime` import
     (an `mprotect` of the slot's page). The sequence, with the steps after it that the
     configuration gates, is `integration/ctor_sequence.h`.

   It opens no socket and does no TLS. `Arm()` can still throw `std::bad_alloc` (it allocates
   outside a `try`). The on-disk log is rotated, never deleted: at open once it is 1 MiB or more,
   and in-process once this run has written 1 MiB; the old file keeps a `nevr-sentinel.<unix_ms>`
   name, so every process start that begins with a full log, and every 1 MiB written, leaves one
   more file. A rename that fails is logged once to logcat and retried at most once per 30 s; the
   log keeps growing meanwhile. Nothing prunes them; whether that is acceptable is the owner's decision. The remaining
   risk is a stall in the storage layer of the headset (FUSE-backed external storage): the
   non-blocking opens do not bound it, and it has not been measured on a device. An
   unknown binary, a failed validation or a partial install leaves the original call intact
   and emits one structured error.
5. **Social.** Portable roster, party and name rules in
   `src/runtime/compat/social_{roster,party,names}.*` are shared with the Windows facade. The 75-slot
   Windows facade and `echovr.exe` offsets are not a Quest ABI: `src/quest/social/` is a Quest
   provider adapter over the same models with the 76-slot Quest vtable (section "Social provider").
   `nevr_social` is declared
   only when the social feature is effective AND the social facade is actually installed;
   otherwise the login carries 0. The shared value is `nevr_social_party::kSocialLevel`
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

Keys. Each of the eight literal `_host` key strings in the two libraries has the xrefs below (`libr15.so` also holds `radserverdb_host` at `0x2ba0b25`, referenced only by `CJson::SetString` at `0x11f871c` and `0x11f87ac`, never read through `TString`); each is
either a `CJson::TString` first argument or a `CJson::SetString` first argument (the latter at `libr15.so`
`0x11f85fc` and `0x11f868c` for `config_host` and `login_host`, `0x11f871c` and `0x11f87ac` for
`radserverdb_host`, which `CR15Game::PreprocessCommandLine` writes). In each `TString` pair the
first call's fallback is the game's built-in `wss://` default and the second call's fallback is the
first call's result, which then goes to `CUriContainer::Parse`.

| Library | Caller (call site) | Key read first, then second | Built-in default (fallback of the first read) |
| --- | --- | --- | --- |
| `libr15.so` | `Initialize` (`0x1286060`, `0x1286078`) | `config_host`, `configservice_host` | `wss://config.readyatdawn.com/rad/rad15_live` |
| `libr15.so` | `VerifyServerLoginConnection` (`0x12508bc`, `0x12508d4`) and `BeginLogIn` (`0x1291b10`, `0x1291b28`) | `login_host`, `loginservice_host` | `wss://login.readyatdawn.com/rad/rad15_live` |
| `libr15.so` | `LogInSuccess` (`0x126cd1c`, `0x126cd34`) and the function at `0x1289ac8` (`0x128a790`, `0x128a7a8`); both addresses lie inside `CR15NetGame::BeginMultiplayer(SR15NetMatchSettings const&)` (`0x128960c`, 9712 bytes) | `transaction_host`, `transactionservice_host` | `wss://transaction.readyatdawn.com/rad/rad15_live` |
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
checked (`libr15.so`, `libpnsradmatchmaking.so`, `libpnsrad.so`; `strings -a | grep '%s.*host'`).

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

Four libraries define `CJson::TString(char const*, char const*, unsigned int)`: `libr15.so` and
`libpnsradmatchmaking.so` (hooked), and `libpnsrad.so` (JUMP_SLOT `0x72bc58`, 49 `bl` and 11 `b` sites)
and `libpnsovr.so` (`0x6db090`, 58 and 5), which are not hooked and read no endpoint key that this
analysis found. Paths that can still reach Ready At Dawn with the redirect on, measured from the ELFs:
- reads made before the slot is hooked (see the caller's contract);
- HTTP (below);
- the unhooked `TString(CSymbol64, char const*)` overload: 15 `bl` sites in `libr15.so` (slot
  `0x36fa288`) and 5 in `libpnsradmatchmaking.so` (`0x6bb458`); four of the `libr15.so` sites are script
  expressions that read config strings by a script-supplied symbol (`CR15NetConfigStringExpression`,
  `CJsonConfigString*`). A search for the endpoint keys' symbol hashes found nothing, but it had no
  positive control, so a script path to an endpoint is not excluded;
- the `TString(void*, char const*)` overload: 4 `bl` sites in `libr15.so` (`0x36e92d0`).
PCVR redirects an `https://...readyatdawn.com` value under any key; Quest redirects values only under the
service keys above.

HTTP does not go through `TString`. `https://api.readyatdawn.com` (`libr15.so` `0x126c270`,
`0x128958c`; the `CreateConnection` calls are at `0x126c274` and `0x1289590`) goes to `CSysHttp::CreateConnection`; so does
`CR15NetStoreTransactions::InitializeHttp`, which reads the key `env` (`0x126c214`) and, when it is
not `live`, builds `https://api-%s.readyatdawn.com` (`0x126c240`) for `CreateConnection`
(`0x126c25c`). `CreateConnection(unsigned long&, char const*)` is defined in `libr15.so` (`0xf96c08`) and
called through its own PLT (JUMP_SLOT `0x36e8028`), the same shape as the `TString` slot, so it has its own
thunk (`Slot::kCreateConnection`, installed with libr15's `TString` slot): a URL that starts `https://api.` or
`https://api-` goes through `ServiceRedirector::ApplyUrl`, the same shared policy and string pool, so it
reaches `nevr_http_uri` (never the bridge) before the original connects; the handle slot, the result and every
other URL pass through. The strings of the game's service-status request
(`status/services,news?env=%s&projectid=rad14`, `libr15.so` `0x2baa0d2`) and of its matchmaker queue API
(`ready_at_dawn/join_queue`, `poll_queue_position`, `leave_queue`, `0x2bad4ea`, `0x2bad390`, `0x2bad547`) are in
`libr15.so`, and the PC build sends both on the connection `InitializeHttp`-style code opens to the same base
URL, so this redirect is what lets nakama answer them on Quest (nevr-runtime#408, #414); which libr15 function
sends each on Quest is not decoded here.

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
bytes, a pool refusal, an exception), counting the fault and never logging (a hooked call does not log). For a
redirected value it returns a pointer from the stable string pool, so the same value always maps to
the same address for the rest of the process. The first sight of each distinct value runs the policy
and may intern one string; up to 16 values are remembered, and the game's four built-in defaults are
resolved when the hooks are installed, so the normal reads allocate nothing. The sentinel installs the
redirect through `InstallRedirectHooksWith` (a bridge probe that reports the loopback listener) from
`integration/production_steps.cpp`. The caller's contract (also in `hook_adapter.h`): register the
counters before `StartReporter`; install from the sentinel's ELF constructor so it precedes
`CR15NetGame::Initialize`, which reads `config_host` at `0x1286060` (`entry.cpp` starts the reporter right after registering the clock counters, so the integrator moves
`RegisterRedirectCounters()` ahead of that `StartReporter`; a value read earlier stays as
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
`CModuleLoader::Load` at `0x3c0a3c`. `libr15.so` reads the module name as
`TString("matchmaking_plugin", "pnsradmatchmaking")` (call at `0x125e8cc`; the key may override the
fallback); the load time is not established.
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

`TryInstallLoginHook` is called by the sentinel from the post-load installs (section
"Integration") with the token-auth `IdentitySource`. The install point is `libr15.so`'s `dlopen` import:
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
writes its globals and continues.

A stand-in is used only when all eight accessor hooks are installed AND the identity source reports
a real NEVR login is ready (`IdentitySource::Ready`, a lock-free atomic load by contract, default
false so an unwired source never stands in); otherwise the callback hooks only measure and the
game's own Oculus login failure runs. The stand-ins (`login_standin.h`) are drawn from the kernel's
random source once per process and recognised again by exact comparison. Being per-process-random,
no two headsets share a value, so a leak cannot link one device to another; within one process a
stand-in still identifies that process, which is why the gate and the resets below keep it off the
wire rather than relying on the value being unguessable. A transient Oculus error (the game's own
`error|is_transient` test, re-implemented as a strict JSON walk over `ovr_Error_GetMessage`) is
passed through so the game's own re-request runs, bounded per attempt by `kMaxTransientPasses` and
`kTransientWindowMs`, then stood in so a permanently-transient error still proceeds.

Whether a login goes out is decided at the send, not from a per-attempt flag — the game keeps a
stand-in across attempts (it re-fetches the org id only at -1, the user name and token only at
`"?"`; only the user proof is requested every attempt). The send gate (`FinishLogin` / `DecideSend`,
`login_rewrite.h`) reads the **current** wire state after the rewrite: the account id
`CNSUser::SendLogInRequest` will send (the virtual `AccountID()`), the JSON `access_token` and the
JSON `nonce`. A login whose account id is a stand-in, or 0 or -1, or whose token or nonce is a
stand-in, is refused whatever the outcome and whether or not NEVR is ready; a clean wire is sent. A
`Rewritten` login carries the NEVR account id, token and nonce (and `SetAccountId` is allowed to
overwrite a stood-in org-id global with the NEVR id — `OculusIdMemory` never remembers the stand-in,
putting back the game's `-1` re-fetch marker instead), so it passes. A refused or declined login
resets the stand-ins the game holds to the game's re-fetch markers (org id to `-1`, OfflineID decimal
`0x70e458` cleared, user name to `"?"`) so the next attempt asks Oculus again, and is reported to the
game through `CNSUser::DeferredLogInFailed` (`0x382e44`) with code 500 and the game's own text "Log in
request failed: One or more prerequisites are missing" (libpnsovr `0x556b40`), which runs on the
game's next update outside the OVR callback. A rewritten login replaces the user-name buffer
`0x70e470` and the OfflineID decimal `0x70e458` (`CNSOVRUser::OfflineID`, `0x1ede20`) with the NEVR
values before the send, since the game writes `0x70e458` only on a real org-id fetch.

A login attempt that is NOT ready additionally runs `ResetHeldStandIns` (on the first prerequisite
callback), which puts any stand-in still in `0x70e3e0` / `0x70e458` / `0x70e470` back to the game's
re-fetch markers. This covers the case where an earlier ready attempt stood values in but the game
then failed the login on its own path (e.g. `GotUserProofCB`'s error branch calls `vtable+0x10`, never
the hooked send `vtable+0x8`), so a stand-in cannot persist across a later not-ready attempt. A real
Oculus value is never a stand-in, so a working-Oculus device is untouched.

Limits that remain (all cleared by restarting the game, which draws fresh stand-ins and starts with
`Ready` true from a cached token):
- The access-token engine string is not rewritten (no safe write proven for it), so after a NEVR
  login a stood-in token stays there, read by the `CR15NetMatchmakerQueue` Join/Heartbeat/Leave URLs
  to `graph.oculus.com`. It is a per-process random value. `quest_login_prerequisites_residual` logs
  it once. The gate still refuses any *login* whose JSON token is a stand-in, so this is a matchmaker
  URL value only, not a login identity.
- `CNSOVRSocial::AddMember` / `JoinInternal` / `FollowDeepLink` (libpnsovr) copy `0x70e3e0` and
  `0x70e470` into the local party member record (`[this+0x2e0][0]`, `[this+0x2f8]`), which
  `CR15NetGame::Party()` includes as entrant 0 for `CNSLobby::RequestCreateSession`. These read the
  globals at the moment they run: after a NEVR login the globals hold the NEVR id and name, so the
  member is correct; they would capture a stand-in only if they ran while the globals still held one,
  i.e. during a ready login's in-flight window and under the unmeasured assumption that the social
  update and the OVR callbacks are on different threads. Party/lobby operations occur after login, and
  `FollowDeepLink` needs a launch-intent deep link, so they do not run in the pre-login window in the
  normal flow. The durable fix is the NEVR social facade (separate package): once its hook is
  installed it hands the game a facade instead of libpnsovr's `CNSOVRSocial` on libr15's
  `CNSProvider::Social` slot (`0x36ef528`), so the Oculus `AddMember` path never runs; as of this
  writing that hook is not yet wired into startup, so the real object is used and this window exists.
- `CNSOVRUser::OfflineID()` (`0x70e458`) is consumed only by `CNSUser::LoadLocalData` / `SaveLocalData`
  / `DeleteLocalData` as one field of a local-storage key and in a log line; it is not sent to the
  server. It is rewritten/reset as above, so it holds a stand-in only in the same in-flight window.

What restarts a login after a failure is not established from the binary: `LogInFailedCB` switches
the game to the "login failed" state and a new login comes from the UI script (`CR15NetBeginLoginNode`
calls `BeginLogIn`); nothing was shown to re-enter login on its own. In both smoke runs the process
stayed alive for over a minute after the failure. The player restarts the game after signing in, and
a cached token makes `Ready` true at the next startup; an automatic in-process retry is not claimed.

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
| Configured legacy account | `/nevr?format=evr&discordid=<percent-encoded id>&password=<percent-encoded password>` with `Authorization: Bearer <public socket server key>` | The ingress admits the upgrade with the server key and Nakama authenticates from the URL credentials. `nevr_serverdb_uri::BuildBridgeCredentialUri` percent-encodes; a password is never concatenated into a URL. With URL credentials present, the bridge chooses the server key over a JWT. |

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
account id (`nevr_social_party::MemberUuid` derives the party UUID from `OVR-ORG-<id>`). On PCVR the enabler is
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
  unless `CNSOVRUser`'s platform word is 4 (the platform the NEVR login carries and `nevr_social_party::MemberUuid`
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
  for go out through `nevr_social_party::SetSender`.
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
  (`nevr_social_party::State::AbandonCreate`, `AbandonJoining`, `ForgetLockRequest`), so the state does not stay
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
- **Sender contract.** `nevr_social_party::Send` returns false when any frame of the batch was refused, and still
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
  reporter's 96 for the whole program.
- **Logging.** Every request logs its name, symbol and whether it was sent, with the ids it carries: the
  account it is aimed at (`target`: the invite target, the kicked, passed or answered member, the profile
  asked about), the party, and the Standard message's subject (`arg`) or a Targeted message's parameter
  (`param`); a Targeted message carries only a UUID derived from the account id, so its builder records the
  account id for the log. No secret is in any of them. The lines go to logcat unless the
  integration installs the sentinel's disk sink (`sentinel::SetLogSink`, PR #220's `sentinel_log.h`), so
  durability is the integration step's to provide; this package does not install one.

Party and member data (headset type per member, the lobby id a non-host member follows) is carried as on the PC:

- **Receive.** `ObserveFrames` takes `SNSPartyDataNotify` (`social_frames.cpp`), checks it is a JSON object and hands
  it to `nevr_social_party::State::ReceiveData`. The first `Update` after it loads it into the game's CJson before the
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
`InstallSocialHook` calls `nevr_social_names::RegisterDefaultDecoder()`. Without that call no profile is requested and
rows show account ids. `social_names_test` decodes the real zstd frame the PC tests use and shows the name on a
friend row.

### The player's path on Quest

Each step, the game function that drives it (libr15) and the facade answer. Slot numbers are Quest's vtable slots and
callback numbers are the indices of `Callback` (`social_abi.h`).

| Step | The game | The facade |
| --- | --- | --- |
| Friends tab opens | `R15NetRefreshFriendsNode` | `RefreshFriends` (46) sends `FriendListRefreshRequest` |
| Rows | `CR15NetGame::FriendId` (0x129b6f8) reads `FriendCount` (47) and `FriendId` (50), then `FriendName` (51), `FriendStatus` (52), `FriendStatusString` (53), `FriendIsInvitable` (54), `FriendPartyId` (56) | the roster fed by `FriendListResponse`, `FriendStatusNotify`, `FriendPresenceNotify`; names from the zstd profile reply |
| "+" on a friend | `R15NetPartySendInviteNode` -> `CR15NetGame::PartySendInvite` (0x129c7d8): the profile gate (hooked true), then a provider comparison (silent exit on mismatch), then `SendInvite` | `SendInviteInternal` (8) sends `PartyInviteRequest`, or a create first |
| The tablet shows a party | `CR15NetGame::Update` passes the "wants a party" flag | `Update` (14) sends `PartyCreateRequest`; `Ready` (21), `Id` (26), `MemberCount` (27), `MemberName` (29), `Host` (24), `IsHost` (25) |
| Friend gets the invite | `PartyInviteReceivedCB` (14); `InviteCount` (71), `InviteSender` (72); accept = `AcceptInvite` (74) -> `JoinInternal` (2), gate `PartyInvitationCB` (7) | `PartyInviteNotify` -> `InviteReceived`; the accept sends `PartyInviteResponse` |
| Member list, headset type | `PartyMemberJoinedCB` (9), `PartyMemberLeftCB` (11), `PartyMemberUpdatedCB` (10); `PartyMemberHeadsetType` (0x129b168) reads the member array at +0x248 | members shown up to the game's 10-entry array; the server's member data loaded into the array before the callbacks; the local member's data (`MemberDataWritable`, 31) shared |
| Party joins a match together | the leader's `CR15NetGame::Update` writes the lobby settings into the party CJson (+0x1f0); a member reads it (`PartyTeam` 0x129a9ac, `PartyData` 0x129aa08) and `PartyUpdatedCB` (3) | the leader's CJson is read out and shared (`PartyDataUpdateRequest`); a member's is loaded from `PartyDataNotify`; `EnterLobby` (32..34) / `ExitLobby` (35) set the lobby fields |
| Join errors | `PartyJoinFailedCB` (0x126f630), codes 1..6 | `JoinFailed` with the server's code; code 0 for a join that could not be sent or timed out |

Game-native, needing nothing from the facade. Voice mute (`SetVoipMuted` 0x1290ed8 sets a lobby-entrant boolean through
`CNSLobby::SetEntrantBoolean`; the voice stream is pnsovr's `Voip*`, which stays) and MUTE ALL / Personal Bubble / Ghost All
(`CR15NetEnableSocialFeatureNode::Enter` 0x23222d4 calls `CR15NetSocialInteractCS::EnableFeature` on game state) never reach
the social object. `RequestProfile` (0x1257fa4) builds its JSON and calls the provider's `CNSIUsers::RequestProfile`, which is
pnsovr's `CNSOVRUsers` (kept), so it works as natively when the reply (`OtherUserProfileSuccess`) reaches the game over its
own login connection; LOBBY INFO (`CR15NetSocialGroups`) listens for `SNSChannelInfoResponse` on the game's TCP broadcaster
the same way. What they need is the router carrying the server's frames to the game and the rewritten login carrying the profile
fields; the facade neither sees nor changes those frames (the observer logs only the social symbols it knows). The
`social_plugin` config block the PC builds is not in any library of the store APK (searched in every `.so`).

What differs from the PC, and why: the object is handed over by a GOT hook on libr15's `CNSProvider::Social` instead of a
patch in `echovr.exe`, with an AArch64/Itanium vtable of 76 slots; every call into the game is isolated in a
`-fno-exceptions` unit over plain-data plans, because the game and the sentinel carry two unwinders; the CJson,
invite-gate and provider calls use libr15 exports at pinned addresses; and the Quest facade has guards the PC lacks (members
past the array hidden, refused and unanswered requests rolled back, a bounded event queue), which the PC reaches through issue
#234 and its own later fixes. Everything else (party model, roster, names, login profile, wire building) is the same source.

Accepted regressions against native Quest, final: Oculus deep links stop (`FollowDeepLink` is reached only through
`CNSOVRSocial::Update`, which the facade replaces); Oculus rich presence advertises NEVR party id, size and joinability
(`CNSIRichPresence::Set` reads the facade, accepted pending the owner's call on zeroing it, which needs a hook this package does
not install); the invitable-users refresh goes away; Oculus friends, invites and the Oculus party overlay no longer reach the game.

### Integration contract

What the integration commit calls, and when:

1. **Install, in the sentinel constructor** (`nevr_sentinel_ctor`, after `InitActivation()`, next to the
   existing GOT hooks): `quest_social::RegisterSocialReportCounters()` before `StartReporter` (it takes 19
   of the reporter's 96 counters; the clock hook takes 2 more, leaving room for the login, redirect and router hooks), then
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
4. **Network adapter:** `nevr_social_party::SetSender(fn)` before the first request can be sent (until then a
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
- Rich presence to Meta (`presence_local`, #396). `CNSOVRRichPresence` stays on pnsovr but its three slots that talk
  to Meta's platform service are wrapped on the same pinned object as the trace: `ShareData` (slot 0, `0x1f044c`,
  `group_presence set`), `RefreshDestinations` (slot 11, `0x1f1c2c`, `get_destinations`) and `Clear` (slot 16,
  `0x1f1cf0`, `group_presence clear`). With the feature on they are answered locally and the object's state word
  (`this+0x30`: bit 0 dirty, bit 1 share in flight, bit 2 clear in flight) is left as the game's own versions leave
  it once the answer has come back (`Clear` sets bit 2 and `ClearUserPresenceCB` takes it down again, so locally it
  stays down), so the game sees a completed share and no request leaves. The status text
  the player sees is the game's member JSON (destination names from the `presence_names` table); a friend's
  status is derived by the game service from the match they are in (nakama `server/evr_friend_presence.go`), so
  nothing is published from the client. Off by default; needs the social facade.
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

## Integration

`src/quest/integration/` wires the packages above into the sentinel entry point. `entry.cpp` calls one
function, `RunSentinelConstructor`, which runs the sequence in `ctor_sequence.cpp` over the real
libraries (`production_steps.cpp`). The sequence is policy over an abstract `Steps`, so
`src/quest/tests/integration_sequence_test.cpp` drives it with fakes.

**Order.** (1) crash reporter; (2) configuration (`InitActivation`); (3) every counter of every hook
that will be installed; (4) the single `StartReporter`; (5) the clock hook; (6) token auth on its own
thread; (7) the sign-in prompt hooks on libr15's `SetDelimitedErrorMessage`, `CR15NetGame::Update` and
`CR15UIPage2EnablePageNode::Enter` slots (#239), wherever token auth is wanted, once it has started, since token auth is what publishes the
prompt; (8) the bridge (loopback listener and router); (9) the `CJson::TString` and `CSysHttp::CreateConnection` redirect on libr15;
(10) the social facade; (11) the hook on libr15's `dlopen` slot, whose post-load login install also
installs the login prerequisites (#240). Counters are registered only for hooks that will be installed:
clock 2, redirect 12, dlopen 1, login 2 (the `SendLogInRequest` thunk's calls and faults, #237), login
prerequisites 16 (one calls counter per hook, #338), social 19, login prompt 14, 66 of the reporter's 96
slots (`integration_hooks_test` runs the sequence against the real registration functions). The production identity source answers the prerequisites'
`IdentitySource::Ready()` from a lock-free `nevr_quest_login::ReadyFlag` (one atomic load, no allocation): the
token-auth poll thread and each `Fetch` set it to whether `Fetch` returns `Ok` for the state they observed
(token auth Ready with an access token and a NEVR account) and clear it in every other state, so a
stand-in Oculus answer is given only when the NEVR rewrite will replace it (#240). It can trail a state
change by up to one poll period (2 s).

**Dependencies.** A failed or skipped piece turns off what needs it and nothing else. Token auth
failing turns off the login prompt, the bridge, the login hook, the social facade and the redirect; the bridge failing
does the same. A redirect that points the game at a loopback port nobody listens on, or straight at a
TLS endpoint the game cannot speak, is worse than the game's own hosts. Refused counters turn off only
their own hook. Every step is contained: a `std::exception` from a step is a `threw` outcome and the
sequence goes on. The constructor never waits on the network or on a thread; the only socket it opens
is the loopback listener, because the redirect needs its port before the game reads `config_host`.

**Login.** The router is built without a login builder. The Quest game sends its own `LoginRequest`
through `CNSUser::SendLogInRequest`, and the login rewrite has already put the NEVR token, the NEVR
account id (the id the token carries), platform code 4 and the game's own HMD serial into it; the
router relays it as the first frame of the login session. A second, injected `LoginRequest` would give
the service two logins on one session while the game still waits for its reply. The remote upgrade
carries the token session's JWT as its Bearer; with no JWT the remote session is not started.

**Post-load installs.** The hook on libr15's `dlopen` slot (`dlopen_hook.cpp`, JUMP_SLOT `0x36c6380`,
measured in the store APK's `libr15.so`) calls the real `dlopen` and, for a non-null handle,
`AfterDlopen` (`post_load.cpp`), which runs the login hook install and the matchmaking redirect install
until each settles: `module_not_loaded` is retried after the next `dlopen`, anything else ends the
action. The handler restores `errno`. `AfterDlopen` runs after the game's call has returned and calls
no game code; it is marked `NEVR_OUTSIDE_GAME_CALL`.

**Social.** `FeatureEnabled(kSocial)` gates the facade (it requires login). The login declares
`nevr_social` only after `InstallSocialHook` succeeded (`Runtime::socialLevel`). The bridge is `quest_net::SessionBridge` (`net/session_bridge.cpp`); its
`Config::tap` (`net/frame_tap.cpp`) sees every relayed frame, which feeds `quest_social::ObserveFrames` and, on the
service's `LoginSuccess` (account id at payload offset 24), `quest_social::SetLocalAccount`. The facade's
requests go out through `nevr_social_party::SetSender` and `SessionBridge::SendToLogin`, a side channel that refuses
until the login is accepted.

**Sensor annotation.** `NEVR_OUTSIDE_GAME_CALL` (`sentinel/outside_game_call.h`) places a function in the
output section `nevr_outside_game_call`. `TestHookFramesCarryNoPersonality` reads that section from the
built library: a function inside it is not checked and its callees are not followed from it, and every
one the walk reaches is listed and compared with a reviewed list (`ComposePlan`, `AfterDlopen`). There
is no name list in the sensor. The annotation asserts that the function calls no game code, lets no
exception out and is not on the stack across a game call; a personality-bearing function that is live
across a game call fails however it is marked. `frames_sensor_test.go` pins this with synthetic graphs
and two linked probes (`frames_probe_ok`, `frames_probe_bad`).

**Stage logs.** One structured line per stage, with a stable `event`, a `status` and a failure `class`
(`stage_log.h`): `config_loaded`, `clock_hook_installed`, `token_auth_state`, `router_listening`,
`redirect_installed`, `social_hook_installed`, `dlopen_hook_installed`, `libpnsovr_loaded`,
`login_hook_installed`, `matchmaking_redirect_installed`, `router_remote_connected`,
`router_remote_failed`, `login_rewritten`, `login_accepted`, `login_refused`. The hook backend's lines
and the stage lines reach logcat (tag `NEVR-Sentinel`) and the on-disk `nevr-sentinel.log`.

**Not run on a headset.** Everything in this section except the sentinel base (loader shim, Breakpad,
clock hook) is built and host-tested only: the order of the constructor against the game's first config
read, the `dlopen` hook, the login rewrite, token auth, the TLS connection to the service, and the
social facade.

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
