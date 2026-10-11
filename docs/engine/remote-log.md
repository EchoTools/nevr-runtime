# Remote logs: the game client, the runtime and the game service

Vocabulary: the **game client** is `echovr.exe` on a player's machine (PC) or `libr15.so` (Quest); the
**game server** is the dedicated `echovr` game server; the **game service** is nakama.

Every claim cites a source. `echovr.exe`/`libr15.so` addresses are from ReVault (project `echovr`, Ghidra
raw decompilation); nakama paths are at `EchoTools/nakama` `origin/main`. A claim without a citation is
labelled *unverified*. Libr15 thunk names in Ghidra carry a +0x100000 skew against the real VA; the VAs
below are the real ones.

## 1. The switch that sends all remote logs

The game service decides, per session, whether the game client sends every category. Three inputs, any one
of which turns it on (`server/evr_pipeline_login.go:838`):

| Input | Where it is set |
| --- | --- |
| `debug=true` in the query of the websocket URL the game client connects with | `server/session_ws.go:201`: `enableAllRemoteLogs: parseUserQueryFunc(&request, "debug", 5, nil) == "true"` |
| the account's `EnableAllRemoteLogs` | `server/evr_account.go:67` (`enable_all_remote_logs`, account profile metadata) |
| the global `EnableSessionDebug` | `server/evr_global_settings.go:128` (`enable_session_debug`, service settings) |

When `params.enableAllRemoteLogs` is true, the login reply's `GameSettings` turns on every category
(`server/evr_pipeline_login.go:176-181`): `RemoteLogSocial`, `RemoteLogWarnings`, `RemoteLogErrors`,
`RemoteLogRichPresence`, `RemoteLogMetrics` all `true`. Otherwise `evr.NewDefaultGameSettings()` sends
`RemoteLogMetrics: true` and the other four `false` (`server/evr/login_settings.go:119-127`). The
`GameSettings` document is the third message of the login reply, after `LoginSuccess` and the unrequire
(`server/evr_pipeline_login.go:205-209`); it is the `SNSLoginSettings` message (`0xed5be2c3632155f1`,
`server/evr/structs.go:69`), a zlib-compressed JSON document (`login_settings.go` `Stream`).

So the runtime-only way to get "all remote logs" is to add `debug=true` to the websocket URL it connects
with. Nakama needs no change. Section 4 lists the two places the runtime builds that URL.

## 2. The game client

### 2.1 Receiving the settings

| Step | echovr.exe | libr15.so |
| --- | --- | --- |
| receiver of `0xed5be2c3632155f1`, logs `[NSUSER] Login settings received: %s`, stores the JSON at `CNSIUsers+0x3a0` | `0x14060c420` (registered at `0x140609e20`) | `CNSIUsers::LoginSettingsResponseCB` `0x1919eb4` (`FUN_01919fd0` stores it) |
| callback on the stored JSON | `0x140181610` (`FUN_1400ff3d0` with the JSON) | `CR15NetGame::LoginSettingsChangedCB` `0x1266f18` -> `NR15Logger::Configure` `0x120b38c` |
| parse of the booleans | `EnableRemoteLogCategories` `0x140104050` | `NR15Logger::EnableRemoteLogCategories` `0x120b474` |

`EnableRemoteLogCategories` (`0x140104050`, read in full) writes one flag word, `DAT_141ffcdf4`
(libr15: `DAT_0376c458`), one bit per key, with the default used when the key is absent:

| bit | key | default when absent |
| --- | --- | --- |
| 0 | `remote_log_errors` | false |
| 1 | `remote_log_warnings` | false |
| 2 | `remote_log_metrics` | true |
| 3 | `remote_log_user` | true |
| 4 | `remote_log_social` | true |
| 5 | `remote_log_rich_presence` | true |
| 6 | `remote_log_iap` | true |
| 7 | `remote_log_store` | true |
| 8 | `remote_log_server_lib` | true |
| 9 | `remote_log_matchmaker_queue` | true |

The flag word's static initial value is `0x3fc` (bytes `fc 03 00 00` at `0x141ffcdf4`): errors and
warnings off, the rest on. Only five of these keys are in nakama's `GameSettings`
(`server/evr/login_settings.go:22-27`); the other five keep the game's default. The flags are rewritten only
when a settings message arrives (`EnableRemoteLogCategories` has one caller, `0x1400ff3d0`, whose only caller
is `0x140181610`).

Seven further integer keys set per-category level masks, read by `0x1400ff3d0` / `Configure`:
`remote_log_user_levels`, `remote_log_social_levels`, `remote_log_rich_presence_levels`,
`remote_log_iap_levels`, `remote_log_store_levels`, `remote_log_server_lib_levels`,
`remote_log_matchmaker_queue_levels` (strings at `0x1416d5220..0x1416d52a8`+; defaults `0xc,0xc,0xc,0xc,0xe,0xe,0xe`
in `DAT_141ffce20..ce38`). Nakama's `GameSettings` does not carry them.

### 2.2 Capturing: the log sink

Every engine log line goes through `CLog::PrintfImpl` `0x1400ebe70`, which calls the registered sinks with
`(mask, category, text)`. The remote-log sink is `0x14011e3d0`, registered by `0x1400ff4b0` with filter
mask `0xf` (libr15: `NR15Logger::RemoteLog` `0x120ada0`, registered by `CR15Game::Initialize` `0x11f4198`).
Read in full:

- category 1..7 lines are dropped unless `mask & level_mask[category]`, and then only when the category's
  flag bit (3..9, above) is set. Category numbering follows the `_levels` key order and flag bit order:
  1 user, 2 social, 3 rich_presence, 4 iap, 5 store, 6 server_lib, 7 matchmaker_queue.
- category 0 lines: a mask-8 line (error) is kept when bit 0 is set, a mask-4 line (warning) when bit 1
  is set; a mask-2 (info) category-0 line is **dropped** (`default: ... if (param_1 != 8) return;`).
- kept lines are appended to the calling thread's buffer: `+0x0` for mask 8, `+0x8090` for mask 4,
  `+0x10120` for mask 2.

The buffers are a per-thread object (TLS key `SRemoteLogs`, `0xd535565095ef8a5d`, accessor `0x1401110a0`).
Buffers 0..2 hold at most 8 records and `0x8000` bytes of string data each (set at `0x1400f4620`, append
`0x1400d4840` returns -1 silently on overflow).

A fourth buffer (`+0x181b0`) takes the JSON records: every `CR15NetJsonLog` object ends in vtable slot 1
(`0x1401ccf40`), which adds `"level":"WARNING"`/`"ERROR"` for mask 4/8, closes the object and calls
`0x140113960`, which appends to `+0x181b0` when the **metrics** bit (bit 2) is set (libr15:
`NR15Logger::MetricLog` `0x120cc50`). Buffer 3 has no size cap that was found.

### 2.3 What writes records (JSON producers)

`CR15NetJsonLog` records are objects with `"message"`, optionally `"message_type"`, and
`"[session][uuid]"`. Producers read in ReVault:

| Producer | VA | `message` / `message_type` |
| --- | --- | --- |
| `LogGameSettings` | `0x14017e780` (from LogIn `0x14017ef10`) | `Game Pause Settings` / `GAME_SETTINGS` |
| `LogSocialAnalytic` | `0x14017fda0` | `Mute User`, `Ghost User`, `Ghost All`, `Mute All`, `Personal Bubble` |
| Load stats | `0x140f39b90` | `Load Stats` / `LOAD_STATS`, keys incl. `[client_load_time]` |
| `UpdateConnectionStatLogging` | `0x1401bfdc0` | per connected slot, every `remote_log_connection_stats\|delay` s (default 60.0) when `remote_log_connection_stats\|enabled` |
| others (names as ReVault gives them) | | FindSocial, GrantMissingUnlocks, RoundOverCB, OnScoreCB, SetErrorMessage, UserDisconnect, VoipDecoded, `FUN_1401933e0`, `FUN_14019f510` |

### 2.4 Flushing and the wire layout

`0x1401c4170` (libr15 `CR15NetGame::UpdateRemoteLogs` `0x125a1e8`; `FlushRemoteLogs` `0x125a2ec` is
`UpdateRemoteLogs(1)`) is called from `CR15NetGame::Update` (force 0) and from `LogOut`, `EndMultiplayer`,
`0x1401933e0` and `0x1401c2650` (force 1). It needs the channel connected (`0x1400ffdc0`). The senders
`0x140118d50` (client) / `0x140119070` (server) send each non-empty buffer when
`DAT_1420a00d4 != 0 && (force || 15.0 <= now - last)`, then clear all four buffers. Flags are not
re-checked at flush: whatever the buffers hold is sent.

One `SNSRemoteLogSetv3` (`0x244b47685187eae1`, sent without the require flag) per non-empty buffer, from
`0x1401206c0` (client; the server variant `0x1401207c0` writes 1 in `+0x34`):

```
header, 0x38 bytes
  +0x00  EvrId, 16 bytes (platform code, account id; from NetGame+0x28d0)
  +0x10  SUuid, 16 bytes (the session id)
  +0x20  16 bytes of text (a zeroed global in the game; DAT_1420a0090)
  +0x30  u32  level word: 8 error, 4 warning, 2 info/metrics
  +0x34  u32  variant: 0 client, 1 server
blob
  u32 count, u32 offsets[count], then count NUL-terminated strings
```

The nakama decoder (`server/evr/login_remotelogset.go`) reads `EvrId` (two u64), four u64 `Unk0..Unk3`
(these are the `SUuid` and the 16 text bytes), a u64 `LogLevel` and a string table
(`StreamStringTable`): 16 + 4*8 + 8 = 0x38 bytes of header followed by the string table, the same layout as
the game's header and blob. The game's u32 level and u32 variant are the low and high halves of nakama's
`LogLevel` (a server-variant set arrives as `1<<32 | level`; `evr.Debug..Any` masks are `0x1..0xF`).
How the broadcaster (`0x140f8a460` -> vtable +0xa8) joins header and blob into the message frame was not
read; the contiguous layout is inferred from the decoder.
`EvrId` production and the `UNK` platform finding are in `docs/reference/remote-log-set-evrid.md`.

pnsrad is not on the path: no `SNSRemoteLogSetv3` id and no `RemoteLog` string in `pnsrad.dll`.
`libpnsrad.so`, `libpnsovr.so` and `libpnsradmatchmaking.so` carry only the exported config-variable
symbols `kEnableRemoteLogging` and `kRemoteLoggingSendSeconds` (no code); libr15 also names `EnableRemoteLogCategory`, `SetRemoteLogChannelLevels`,
`SetRemoteLogService`, `ResetRemoteLogs` (`0x120c080`). The interval constant in libr15
`ProcessRemoteLogs` (`0x120b8a4` / `0x120c2e0`) was not decoded; *unverified* that it is 15.0 there.

## 3. The game service (nakama)

| Step | Where |
| --- | --- |
| message dispatch | `server/evr_pipeline.go:577` `case *evr.RemoteLogSet` -> `p.remoteLogSetv3` |
| entry, bounded concurrency (16), drop at capacity | `server/evr_pipeline_login.go:1652-1667` |
| trim and filter | `server/evr_remotelogset.go:69` `processRemoteLogSets` |
| per-user journal | `server/evr_remotelog_journal.go`: storage collection `RemoteLogs`, key `journal`, written after the session closes (+10 s) or by a 30 s sweep |
| event | `server/evr_runtime_event_remotelogset.go:95` `EventRemoteLogSet.Process` |

Limits and filters (`server/evr_remotelogset.go`): at most 50 entries and 256 KiB per set
(`maxRemoteLogEntries`, `maxRemoteLogPayload`). A log string is dropped if it contains any of these
`"key":"value"` substrings: `message` in {`Podium Interaction`, `Customization Item Preview`,
`Customization Item Equip`, `Confirmation Panel Press`, `server library loaded`,
`r15 net game error message`, `cst_usage_metrics`, `purchasing item`, `Tutorial progress`}, `category` in
{`iap`, `rich_presence`, `social`}, `message_type` `OVR_IAP`. The `remote_logs_filter` global setting
(`server/evr_global_settings.go:100`) is validated by the settings RPC but is not read by
`filterRemoteLogs` (searched `RemoteLogFilters` in `server/`: only `evr_global_settings.go` and
`evr_runtime_rpc_service_settings.go`).

`Process` drops sessions with a nil user id, bot users, then `UnmarshalRemoteLog`
(`server/evr/login_remotelogset_messages.go:55`): each string must be JSON; the object is read as
`GenericRemoteLog` (`message`, `message_type`, `userid`) and, when its `message_type` or `message` equals
a known name, re-read into a typed struct. Known names: `CUSTOMIZATION_METRICS_PAYLOAD`,
`STORE_METRICS_PAYLOAD`, `Server connection failed`, `Disconnected from server due to timeout`,
`VOIP_LOUDNESS`, `THREAT_SCANNER`, `PLAYER_DEATH`, `NET_EXPLOSION`, `ROUND_OVER`, `GEAR_STATS_PER_ROUND`,
`GOAL`, the `POST_MATCH_*` family, `LOAD_STATS`, `REPAIR_MATRIX`, `SESSION_STARTED`, `USER_DISCONNECT`,
`USER_DISPLAY_NAME_MISMATCH`, `ENERGY_BARRIER`, `FIND_NEW_LOBBY`, `GAME_SETTINGS`, `GHOST_ALL`,
`GHOST_USER`, `PERSONAL_BUBBLE`, `MUTE_ALL`, `MUTE_USER`. An unknown name stays a `GenericRemoteLog`:
it is not an error, it is not acted on, and it is kept by the journal.

Triggers precedent: `RemoteLogLoadStats` with `ClientLoadTime > 45` posts a Discord notice to the guild's
operator channel (`server/evr_runtime_event_remotelogset.go:556`).

## 4. The runtime

- The game's own remote logs pass through the bridges as bytes: the runtime does not parse or rewrite them.
  The places that name `SNSRemoteLogSetv3`: `src/runtime/compat/evr_codec.h` (`kSymRemoteLogSet`, "no require
  flag", and `BuildRemoteLogSet`, the frame the self-checks send) and `src/runtime/compat/session_router.cpp`
  `RequestRaisesRequireCount` (a login-connection send of it does not raise the router's outstanding-request
  count).
- `docs/reference/legacy-translator.md`: the v2/v3 header words are not identified.
- The runtime originates remote logs only for the self-checks (section 6).

Where the websocket URL is built, and what it carries:

| Platform | Where | Query |
| --- | --- | --- |
| PC | `src/runtime/compat/ws_bridge.cpp`: `remoteUrl = g_remoteUri`, then `nevr_serverdb_uri::BuildBridgeCredentialUri` appends `discordid`/`password`; the login connection (index 1) of a self-check build then gets `debug=true` (`AppendRemoteDebugParam`); for connections >= 2 (matchmaker) `format=evr` is removed | the configured `socket_uri` (may already carry `format=evr`, `token`), plus the credentials when configured, plus `debug=true` on the login connection of a self-check build |
| Quest | `src/quest/net/session_bridge.cpp` `SessionBridge::BuildRequest`: the configured `remoteUri`, the same `BuildBridgeCredentialUri`, then `AppendRemoteDebugParam` on a login-role request when `Config::remoteDebugQuery` is set (the `self_check` feature) | same |

`AppendQuery` (`src/runtime/server/serverdb_uri.cpp`) is the percent-encoding query builder both use.

Senders for a runtime-built message on the login connection: PC `SendFrameToServer`
(`ws_bridge.cpp`, queues while the remote is connecting; also the party requests' sender), Quest
`SessionBridge::SendToLogin` (refused until `LoginSuccess` has been seen).

## 5. Whole-project search

Project `echovr` in ReVault holds 1042 binaries: `echovr.exe`, the 9 named ones, and ~1030 script DLLs
and their Quest `.so` twins (hash names, `lib<hash>.so`). Terms: `remotelog`, `remote_log`, `remote log`
(case-insensitive string search, limit 40) and the code regex `RemoteLog|remote_log|SRemoteLogs` (limit
20), run one binary at a time, with a positive control (`remotelog` on `echovr.exe` returns
`SNSRemoteLogSetv3` at `0x1416d4fa3`).

**Searched 1042 of 1042 binaries.** By first character of the name (DLL `x*`, SO `libx*`): 0:39, 1:74, 2:55,
3:72, 4:48, 5:64, 6:49, 7:52, 8:55, 9:66, a:66, b:74, c:93, d:75, e:87 (incl. `echovr.exe`), f:64, named
set 9.

Hits are in three places only; no script DLL or Quest twin names a remote log:

| Binary | Hits |
| --- | --- |
| `echovr.exe` | strings `SNSRemoteLogSetv3` `0x1416d4fa3`, `SRemoteLogs` `0x1416d4fd0`; the 17 `remote_log_*` config key strings `0x1416d5130..`; `remote_log_connection_stats\|enabled/delay` `0x1416dca28`/`0x1416dca5f`; code: `EnableRemoteLogCategories` `0x140104050`, sink `0x14011e3d0`, senders `0x140118d50`/`0x140119070`, builders `0x1401206c0`/`0x1401207c0` |
| `libr15.so` | `SNSRemoteLogSetv3::Send` `0x120bf00`/`0x120c90c`, `NR15Logger::RemoteLog` `0x120ada0`, `Configure` `0x120b38c`, `EnableRemoteLogCategories` `0x120b474`, `SetRemoteLogChannelLevels` `0x120b66c`, `EnableRemoteLogCategory` `0x120b74c`, `SetRemoteLogService` `0x120b88c`, `ProcessRemoteLogs` `0x120b8a4`/`0x120c2e0`, `ResetRemoteLogs` `0x120c080`, `CR15NetGame::UpdateRemoteLogs` `0x125a1e8`, `FlushRemoteLogs` `0x125a2ec`; 17 `remote_log_*` key strings from `0x2ba1fb3`; connection stats keys `0x2ba7248`/`0x2ba7277` |
| `libpnsovr.so`, `libpnsrad.so`, `libpnsradmatchmaking.so` | only the exported config-variable symbols `NRadEngine::kEnableRemoteLogging` and `NRadEngine::kRemoteLoggingSendSeconds` (no code) |

No hit in `pnsdemo.dll`, `pnsovr.dll`, `pnsrad.dll`, `pnsradmatchmaking.dll`, `test-foo.so` or any hash-named
binary.

## 6. Self-checks

A release candidate reports its own run-card checks through this path (`src/runtime/compat/self_check.h`, one
source for the PC runtime and the Quest sentinel).

- **On:** PC when the build is a release candidate (`NEVR_RC_LABEL` set, or `-DNEVR_SELF_CHECKS=ON`); Quest when
  the `self_check` feature is on (the release candidate's default features, or `features.self_check` in
  `nevr-quest.json`; it needs `login`). Off otherwise: nothing is registered, logged or sent, and no
  `debug=true` is added. Never on a dedicated game server.
- **Connection:** the login connection's upgrade query carries `debug=true` (PC `ws_bridge.cpp`, Quest
  `SessionBridge::BuildRequest`), which section 1 turns into every `remote_log_*` category.
- **Sender:** PC `SendFrameToServer` (the login pair), Quest `SessionBridge::SendToLogin`. Nothing is sent
  before `LoginSuccess` (the service drops remote logs from a session with no user); results wait in a queue
  of 64 and the frame carries the user the service named.
- **One result, one string:** `{"message":"nevr_self_check","message_type":"NEVR_SELF_CHECK","userid":...,
  "check":...,"pass":...,"expected":...,"observed":...,"seq":...,"build":...}`; each text is cut at 160 bytes.
  At most 8 results per check per session, then one with `observed":"capped"`; at most 16 strings per frame.
  The same result is written to the build's own log at Info: PC `[NEVR.SELFCHECK] check=... pass=...`, Quest
  `self_check {check, pass, expected, observed}`.
- **Adding a check** is one registration and one call at the event:

  ```cpp
  static const auto kCheck = nevr_self_check::Register({"party_data_share", "the answer is PartyUpdateSuccess", nullptr});
  // ... at the event:
  nevr_self_check::Report(kCheck, "PartyUpdateFailure", /*pass=*/false);
  ```

  An event seen only from a hooked game function, a loader callback or an `-fno-exceptions` translation unit
  does not call `Report`: it bumps an atomic it already owns and registers a `probe`, which `Flush` (the frame
  tick on PC, the 2 s token-auth poll on Quest) calls on a thread that may allocate.

| Check | PC | Quest |
| --- | --- | --- |
| `matchmaking_reload_patch` / `matchmaking_reload_redirect` (#18) | `pnsrad_enabler.cpp` `OnDllLoaded` feeds `nevr_matchmaker_host_patch::ReloadLedger`; passes when every load of `pnsradmatchmaking.dll` was patched | `post_load.cpp` `MatchmakingImages` (distinct handles the game's `dlopen` returned) against the redirect installs in `production_steps.cpp`; passes when installs >= images. A reload mapped at the same address returns the same handle and is not seen |

Run-card checks not registered yet are listed in issue #451.

## 7. What is not established

- The broadcaster's own joining of header and blob (`0x140f8a460` vtable +0xa8): the contiguous layout is inferred from nakama's decoder.
- Whether a dedicated game server ever receives `SNSLoginSettings` (the server flush `0x140119070` reads
  `server_id` from config in `0x1401c4170`).
- The libr15 flush interval constant; the libr15 server-variant flush callers.
- The `+0x181b0` buffer's size cap, if any.
- That the game client honours `debug=true` end to end in a real session: the chain above is static, no
  run has been read for it. The check that settles it is in the nevr log: the `[NSUSER] Login settings
  received: {...}` line must show `remote_log_errors":true` and `remote_log_warnings":true` for a session
  that connected with `debug=true`.
