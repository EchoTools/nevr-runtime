# XPID / login-identity trace: client → wire → Nakama

**Question this answers:** how does a player's XPID (`<Platform>-<AccountId>`,
e.g. `OVR-ORG-1492861453923913968`) get set end to end during login, which of
our layers can override which, and why did a live login attempt show Nakama
logging `evrid=UNK-1` for the login session?

**Status: traced, one confirmed bug, one still-open thread. Nothing in this
document has been fixed — this is the trace only**, produced 2026-09-13 while
diagnosing a client login stall (`./launch-client.sh` hangs at "logging in").
See also [`provider-prefix-slots.md`](provider-prefix-slots.md), which measures
the same string tables from a different angle (patch-byte budget, not
authentication).

Every claim below is cited. "Unverified" means exactly that — checked as far
as time allowed, not further.

## Three separate numbering schemes exist for the same 7 platforms

This is the root of the confusion. Same platform, three different integers,
in three different places, only one of which two other places agree on.

| Platform | Game-internal nibble (`GetProviderPrefix`/`GetUserIDString`, 1-indexed) | Our wire encoding (`ws_bridge.cpp`, what we send) | Nakama's Go enum (`core_account_evrid.go`, what it means server-side) |
| --- | --- | --- | --- |
| STM | 1 | 0 | 0 |
| DSC/PSN | 2 | 1 | 1 |
| XBX | 3 | 2 | 2 |
| **OVR_ORG** | **4** | **4** | **3** |
| **OVR** | **5** | **3** | **4** |
| BOT | 6 | 5 | 5 |
| DMO | 7 | 6 | 6 |

- Game-internal (1-indexed): commit `f84f4565609ef97a197a18c9b1d45224d9b38a8e`
  ("fix: detour GetProviderPrefix to always return OVR-ORG") message: *"Game's
  internal provider numbering differs from Nakama: 1=STM 2=PSN 3=XBX
  4=OVR-ORG 5=OVR 6=BOT 7=DMO"*. Independently confirmed by decompiling
  `GetProviderPrefix` (echovr.exe `0x14060d640`) and `GetUserIDString`
  (echovr.exe `0x1401ba630`) via ReVault — both `switch(*param & 0xf)` over
  cases 1–7 with a shared `default`, both reading the same static string-table
  addresses `0x1416d7134`…`0x1416d7150` (see "Two independent choke-points"
  below). echovr.exe build: `goldmaster 631547` (`//rad/rad15_live('r')`, per
  the r14 client log `Build:` line), sha256
  `b6d08277e5846900c81004b64b298df6acba834b69700a640b758bda94a52043`.
- Our wire encoding: `src/runtime/compat/ws_bridge.cpp:143-154`
  (`PlatformPrefix`) and `:168-172` (`SelectPlatformCode`), both at HEAD
  `59bd220dc4f3ee99dfc119c0864c1dc482e04f87` (last commit touching this file).
- Nakama's enum: `~/src/nakama` (local checkout of the fork this server runs,
  `replace github.com/heroiclabs/nakama/v3 => ../nakama` in `evrcat/go.mod`),
  commit `d725451f7`, `server/evr/core_account_evrid.go:15-25`
  (`const ( STM PlatformCode = iota; DSC; XBX; OVR_ORG; OVR; BOT; DMO )`).
  **Unverified: whether this exact commit is what's deployed on
  `fortytwo.echovrce.com` right now** — Andrew updated that server at
  05:52 CDT today; I did not check its running build/version against this
  checkout.

**OVR and OVR_ORG are swapped between our wire encoding and Nakama's enum.**
This was not always the case — see "The confirmed bug" below.

## Layering: what writes the identity, in what order

Boot order, per `src/runtime/lifecycle/initialize.cpp` and `boot.cpp` (both
touched by `f84f4565609ef97a197a18c9b1d45224d9b38a8e` / `a692a304a79a75564f6b660ea81a96137b433357`
respectively) and confirmed live from `nevr-boot.jsonl` for run
`20-1dd436f94520e9c` (this session's 11:04:00 UTC test):

1. **`Initialize()` — DllMain time, before the game parses argv, before
   `PreprocessCommandLine` is even hooked.**
   - `PatchDscProvider()` (`initialize.cpp:257` → `xpid_patch.cpp:32`):
     rewrites 5 static string-table sites (PSN→DSC, ???→DSC fallback). Runs
     first. Live: `nevr-boot.jsonl` line 11943, `"[NEVR.XPID] DSC provider
     patch applied at 5 sites"`.
   - `PatchProviderPrefixOvrOrg()` (`initialize.cpp:259` → `xpid_patch.cpp:112`):
     hooks `GetProviderPrefix` (echovr.exe `0x14060d640`) to unconditionally
     return the OVR-ORG string pointer. Live: line 11945, `"[NEVR.XPID]
     GetProviderPrefix detour OK"`.
2. **`RunDeferredRuntimeBootstrap()` — fires once, on the game's first real
   `PreprocessCommandLine` call (post D3D12 device creation).** Live sequence
   for this run, `nevr-2026-09-13T11-04-00.121.jsonl`:
   - `platform_compat` module loads (line 34-35, `ts=00.647`).
   - `token_auth` module loads (line 39-40, `ts=00.791`) — **this is when
     `TokenAuth_GetDiscordId()`/`TokenAuth_GetToken()` become callable via
     `ResolveModuleProc`** (`ws_bridge.cpp:463-464`).
   - `PnsradEnabler::Init()` (`boot.cpp:403`) runs (line 42-45, `ts=00.792`):
     patches echovr.exe's own `"pnsovr"`/`"pnsdemo"` string literals to
     `"pnsrad"` so the game's module loader asks for `pnsrad.dll` instead of
     `pnsovr.dll`, and NOPs the OVR-platform branch check
     (`src/runtime/patch/pnsrad_enabler.cpp:179-243`, last touched
     `0dbfee6b6a5dc2d0370a050592bcb5d1926707db`). **This does not itself write
     any account identity** — it only redirects which DLL loads.
   - WS bridge proxy starts (line 50-51, `ts=00.798`).
   - `pnsrad.dll` itself finishes loading and self-patches — confirmed live,
     line 70, `ts=01.908`: `"[EVR] [pnsrad] module patches: 3 succeeded, 0
     failed — social layer (friends/party/login) ENABLED"`. **This is ~1.1s
     before the login websocket even opens** (line 100, `ts=03.266`) — so in
     this run, pnsrad.dll being not-yet-loaded is NOT what breaks the later
     `GetModuleHandleA("pnsrad.dll")` check (see below). The comment at
     `ws_bridge.cpp:537-538` is explicit about why our own injection exists at
     all: *"pnsrad.dll won't send its own [LoginRequest] because it has no
     user identity (OVR SDK is bypassed)."* — pnsrad is the base social layer,
     but with no real Oculus platform underneath it, it has nothing to log in
     with; **our code is what supplies the identity.**
3. **Login-connection Open handler (`ws_bridge.cpp:530-601`), fires when the
   proxied login websocket connects** — this is where `token_auth`'s
   `discordId` overrides whatever pnsrad/CNSUser had:
   - Reads `TokenAuth_GetToken()` / `TokenAuth_GetDiscordId()` /
     `TokenAuth_GetUsername()` (`:463-477`).
   - Falls back to `config.yaml identity.discord_id` if the token gave 0
     (`:487-497`, N20/N133 per the inline comments).
   - If `connIdx == 1` (the login connection specifically) and not already
     injected (`:544`): looks up `pnsrad.dll`'s `Users()` export
     (`:552-556`), and if that succeeds, **directly pokes CNSUser's fields**
     (`:563-577`): `+0x88` = account_id (our discordId), `+0x90` low nibble =
     `4` ("OVR_ORG (game numbering)" — this is the **game-internal** 4, i.e.
     correct per that scheme), `+0x9c` = `0x04` (connected/logged-in flag).
     The comment at `:540-543` explains why: *"Before injecting, set the
     CNSUser's login state to 'logging in' so that CNSUser::LogInSuccessCB
     processes the server's LoginSuccess response. Without this,
     LogInSuccessCB silently discards the message because the user's login
     state at +0x90 is still 0 (logged out)."*
   - Then calls `SelectPlatformCode(hasUrlCreds, g_noOvr)` (`:592`) and
     `BuildLoginRequest(discordId, platformCode, ...)` (`:595`), and sends it:
     `pairPtr->remoteWs->sendBinary(loginMsg)` (`:596`). **This is the wire
     value affected by the scheme mismatch, not the CNSUser nibble poke above
     — the CNSUser poke and the wire-sent platformCode are computed
     separately and use different numbering on purpose** (game-internal for
     the local struct, "Nakama iota" for the wire, per the file's own
     comments at `:177` vs `:169`).

So: **pnsrad brings the social/login scaffolding and bypasses OVR, then
token_auth's discordId is what our code injects as the actual identity on
top of it** — confirming what Andrew described. The override point is the
Open-handler in `ws_bridge.cpp`, not a separate later pass.

## Two independent XPID choke-points client-side — only one is hooked

`f84f4565609ef97a197a18c9b1d45224d9b38a8e`'s commit message calls
`GetProviderPrefix` "the single choke-point for every xpid string — 14
callers." **Verified false by ReVault.** There are two:

1. `GetProviderPrefix` (echovr.exe `0x14060d640`) — **14 real callers**
   (`revault_callers`, echovr.exe): `CNSIUsers::CreateUser`, two `Send`
   overloads at `0x14060e380`/`0x140613900`/`0x140618480`, `SaveLocalData`,
   `Inspect<CBindingsOffsetOfInspector<float>>`, and 6 unnamed `fcn.*`/`FUN_*`
   functions. **Hooked** by `PatchProviderPrefixOvrOrg()` — always returns
   OVR-ORG for these callers, regardless of what's in CNSUser.
2. `GetUserIDString` (echovr.exe `0x1401ba630`, unnamed in ReVault before this
   session) — **40 callers**, entirely separate from #1: `AddBotUser`,
   `AddPlayerUser`, `AddRemoteUser`, `SendProfileUpdate`, `RoundOverCB`,
   `LogSocialAnalytic` (×2 call sites), `ProcessPostMatchBattlePassXp` (×2),
   `FindSocial`, and more. Does its **own** `switch(*param_1 & 0xf)` over the
   same static string addresses and formats `"%s-%llu"` itself
   (`FUN_1400e4730`). **Never calls `GetProviderPrefix`, and is untouched by
   its hook.** This matches an earlier session's memory note, *"network xpid
   still XBX — cached in CNSUser bypasses detour (open)"* — now pinned to an
   exact address instead of a description.

**The `OVR-ORG-1` / `UNK-1` pair Andrew asked about is from #2, and is
unrelated to the login handshake — ruled out, not just assumed:**
- `[NSUSER] Creating user OVR-ORG-1"` (`nevr-2026-09-13T11-04-00.121.jsonl`
  line 97, `ts=03.141`) fires **before** the login websocket even establishes
  (line 100, `ts=03.266`) — 125ms earlier.
- Inspecting one of `GetUserIDString`'s 40 callers, `AddPlayerUser` (echovr.exe
  `0x140152d80`): its log line is `"[NETGAME] Adding player user to slot %llu,
  idx 0"` — the argument formatted through `GetUserIDString` is a **local
  player slot index**, not a discord ID or Nakama UID. The real login
  handshake identifies itself via `discord_id=1492861453923913968` in the URL
  query string — a completely different, much larger number.
- Nakama's `evrid=UNK-1` for the login session
  (`sid=96860a04-af61-11f1-b589-383ec87527a1`) is the server's own rendering
  (`PlatformCode.Abbrevation()`'s `default` case, `core_account_evrid.go:212`,
  returns `"UNK"`) of *some* EvrId it decoded, appearing in a routine
  `*evr.RemoteLogSet` telemetry message (`log_level=2, num_logs=1`) — not a
  login/auth message. **Unverified: exactly which client-side call
  constructs the EvrId embedded in that specific RemoteLogSet** — not one of
  the callers inspected so far (`AddPlayerUser`, `LogSocialAnalytic`) matches
  a network-send path for it specifically.

Conclusion: this pairing is a **red herring** for the login stall. Flagged as
open in the first draft of this investigation; closed here by the timing
evidence above.

## The confirmed bug: platformCode 3/4 were swapped on the wire, on purpose, based on a since-unverified assumption

`src/runtime/compat/ws_bridge.cpp` currently sends wire value **4** for the
URL-credential (discord_id+password) login path, labeling it "OVR_ORG"
(`:169`) — but Nakama's actual enum (`core_account_evrid.go:15-25`, confirmed
above) has **OVR_ORG=3, OVR=4**. Wire value 4 decodes server-side as **OVR**,
not OVR_ORG.

This was not an oversight — it was a deliberate change, with its own test
coverage, at commit `9264ea0dc992f6c4d9d1cacad8552dec37a2e450`
("fix: use game's platform numbering (OVR_ORG=4) on the wire", 2026-08-04),
which flipped `SelectPlatformCode`'s return from 3 to 4 and swapped
`PlatformPrefix`'s case 3/4 labels to match, updating
`test_behavioral.cpp`'s expectations in lockstep (so `just verify` stayed
green). Its stated reasoning: *"The Nakama server echoes the LoginRequest
PlatformCode directly into LoginSuccess (evr_pipeline_login.go:185) — it does
not remap. The game then interprets the value using its own internal
numbering where OVR_ORG=4, not the Nakama enum where OVR_ORG=3."*

**The echo claim is correct** — verified in `~/src/nakama` (commit
`d725451f7`), `server/evr_pipeline_login.go:204`:
`evr.NewLoginSuccess(session.id, request.XPID)` passes the request's XPID
through unmodified.

**But the echo isn't the only thing Nakama does with it before that point,
and this is the part `9264ea0` didn't account for.**
`server/evr_pipeline_login.go:154`: `GetUserIDByDeviceID(ctx, p.db,
request.XPID.String())` — the incoming XPID is formatted to a string (via
`EvrId.String()` → `Token()` → `PlatformCode.Abbrevation() + "-" + AccountId`,
`core_account_evrid.go:101-108`) and used as the **primary key for a device-ID
account lookup**: `server/evr_runtime.go:652-670`,
`SELECT ud.user_id FROM user_device ud WHERE ud.id = $1`. If wire value 4
renders as `"OVR-1492861453923913968"` (per Nakama's enum) but the account
this player actually registered under is keyed
`"OVR-ORG-1492861453923913968"` (platformCode 3), the query returns
`sql.ErrNoRows` → `GetUserIDByDeviceID` returns `codes.NotFound` →
`evr_pipeline_login.go:160-162` calls `formatLoginErrorMessage` and sends
`evr.NewLoginFailure(...)` back to the client instead of a success.

**This is a complete, cited mechanism by which the platformCode swap could
silently fail a URL-credential login — a `LoginFailure` the client may not
surface visibly, leaving NetGame stuck at "logging in" exactly as observed.**
It is not yet proven to be *the* cause of this session's specific stall (see
next section — a more basic problem was also found), but it is a real,
reproducible mismatch that should be fixed independent of whatever else is
wrong, and it's been sitting there, tested and green, since 2026-08-04.

**Unverified:** whether this account's `user_device` row is actually keyed
`OVR-ORG-...` (vs. some other scheme) — I did not query the production DB.

## Still open: our own login injection may not be firing at all

Independent of the encoding bug above, this session captured three live
client runs (`nevr-2026-09-13T10-43-19`, `T10-55-06`, `T11-04-00`, all
against Nakama, the last one after Andrew's 05:52 CDT server update). In
**none of them** does the client's own log contain the line
`"[NEVR.WS] login injected xpid=..."` that `ws_bridge.cpp:598-600` emits
immediately after `sendBinary(loginMsg)` on the injection path described
above. Grepped directly: `grep -n 'login injected'` against the full
per-run JSONL log returns nothing in any of the three runs.

Cross-checked server-side: Nakama's log for the exact login session
(`sid=96860a04-af61-11f1-b589-383ec87527a1`, the `T11-04-00` run) contains
only two lines for that `sid` — the WebSocket connect, and one
`*evr.RemoteLogSet`. **No `*evr.LoginRequest` was ever logged as received**
for that session, even though Nakama does log every `*evr.LoginRequest` it
receives at `debug` level (confirmed: 6161 such lines exist in the log from
other real clients, e.g. `evr.LoginRequest(Session=..., XPID=OVR-ORG-35331,
...)` — a well-formed one, for comparison).

**This means the injection code path at `ws_bridge.cpp:544` may not be
executing at all for this build's login connection** — a different, more
fundamental problem than the encoding swap above, and not yet explained. If
the LoginRequest is never sent, the platformCode-3/4 mismatch is moot for
this specific failure; a request that never goes out can't be rejected for
carrying the wrong platform code.

**Not yet investigated:** why the `if (connIdx == 1 && ...)` gate (or
something upstream of it in the Open handler) isn't firing for the observed
login connection, despite `g_connectionCount++`-based numbering
(`ws_bridge.cpp:351`) appearing, on paper, to assign it `connIdx=1` (config
connection first = 0, login connection second = 1, matching the two
connections actually observed in the client log).

**Confirmed not build- or token-fix-dependent.** Built HEAD (`e0662fd`,
includes the Sep 7 token-auth fixes `3c4703f`/`e0662fd` absent from the
previously-deployed `3.2.0+654.a692a30`) in a separate worktree
(`/home/andrew/src/nevr-runtime-wt-head`), deployed it, and re-ran the same
test against the same (further-updated) Nakama. Identical result: client log
`nevr-2026-09-13T11-15-12.240.jsonl` shows the login socket established
(`ts=15.520`) and no `"login injected"` line anywhere in the run; server log
shows session `sid=6536fbb9-af64-11f1-b589-383ec87527a1` connecting with the
same `discord_id`/`password` query at `ts=15.488`, then only one
`*evr.RemoteLogSet{evr_id=UNK-1,...}` at `ts=29.033` — no `*evr.LoginRequest`
ever received, same as every prior run. Four runs total (2 pre-update Nakama
on the Aug 6 build, 1 post-update on the Aug 6 build, 1 post-update on HEAD)
all show the identical non-firing pattern. **This rules out both the Nakama
update and the Sep 7 token-auth fixes as the cause** — whatever's blocking
the injection is present in both builds and unaffected by the server change.

## Summary table — who can set what, and whether it's verified

| Layer | Writes | Numbering used | Verified this session? |
| --- | --- | --- | --- |
| `PatchDscProvider` (`xpid_patch.cpp:32`) | 5 static string-table bytes | n/a (string rewrite) | Yes — boot log line 11943 |
| `PatchProviderPrefixOvrOrg` (`xpid_patch.cpp:112`) | `GetProviderPrefix`'s return value | game-internal | Yes — boot log line 11945; confirmed only 14 of 54 total callers covered |
| `PnsradEnabler::Init` (`pnsrad_enabler.cpp:179`) | which DLL loads (`pnsovr`→`pnsrad`) | n/a | Yes — client log lines 42-44 |
| `pnsrad.dll` itself (native, loaded ~1.1s before login socket opens) | its own internal social/login state | pnsrad-internal (not traced this session) | Loaded-by-time confirmed; internal behavior **unverified** |
| `ws_bridge.cpp` CNSUser poke (`:563-577`) | CNSUser `+0x88`/`+0x90`/`+0x9c` | game-internal (4=OVR_ORG, correct) | Code read; **not confirmed to execute** in the 3 captured runs (see above) |
| `ws_bridge.cpp` wire LoginRequest (`:267`, `AppendLE64(payload, platformCode)`) | the actual bytes Nakama receives | **Nakama iota, but currently sent with the game-internal value (4) for OVR_ORG** — the confirmed bug | Code read, confirmed by decompiling the Go source; **not confirmed to actually be sent** in the 3 captured runs |
| Nakama `GetUserIDByDeviceID` (`evr_runtime.go:652`) | which account (if any) resolves | Nakama iota, via `EvrId.String()` | Code read in `~/src/nakama`; **deployed-server match unverified** |

## Next steps (not taken — Andrew's call)

1. Confirm whether `~/src/nakama` HEAD (`d725451f7`) matches what's deployed
   on `fortytwo.echovrce.com`.
2. Instrument (temporarily) why the `connIdx==1` login-injection Open handler
   isn't logging — is it not firing, is `connIdx` not 1, or is
   `pairPtr->loginInjected` somehow already true?
3. Once injection is confirmed firing, decide whether to fix the
   `SelectPlatformCode`/`PlatformPrefix` 3/4 swap back to match Nakama's
   actual enum (undoing `9264ea0`), and re-verify against
   `evr_pipeline_login.go:154`'s device-ID lookup specifically, not just the
   LoginSuccess echo `9264ea0` checked against.
4. Find which of `GetUserIDString`'s 40 callers actually constructs the
   `RemoteLogSet` evr_id — not yet identified (see "red herring" section).
