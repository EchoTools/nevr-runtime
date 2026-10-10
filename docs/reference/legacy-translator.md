# Legacy message translator

`src/runtime/compat/legacy_codec.{h,cpp}` rewrites one message at a time between the message family a legacy
game client speaks (the b2 lobby builds: Summer 2019 and Halloween 2018) and the current family the game
service speaks. The game service only ever sees current messages.

It is a set of pure functions. A result depends on the symbol, the payload, the `Context` (login session,
own user id, channel, the bridge's login profile JSON) and the `BuildTables` (the symbols that differ between
the two generations of the b2 lobby message set, and the build's symbol dictionary). It includes no Windows
header and holds no state, so `src/runtime/tests/test_legacy_codec.cpp` runs it on any host. Nothing in
`BugSplat64.dll` calls it yet.

## Sources, and how far to trust them

Each row below names where its layout came from. There are three sources, and only the last is a check
against the game's own binary, and then only of names:

- **EchoRelay documentation** (legacy side). Byte layouts, field order and login result values were learned
  by reading EchoRelay's message documentation and test fixtures: wire facts only, written fresh here.
  **None has been verified against a legacy binary or a live packet.** When the legacy exes arrive, every
  row with this source is to be checked against them.
- **Game service decode order** (current side). What the game service's codec (nakama, `server/evr/*.go`)
  reads today, taken read-only.
- **Our symbol corpus.** `src/runtime/hook/symbol_corpus.cpp` and `src/runtime/log/symcache_data.cpp` come
  from the final build's binary. Rows whose symbol names appear there are marked; the other legacy symbols
  were removed with their messages, so their names come from EchoRelay and the values are the CSymbol64 of
  the name (checked by the tests).

## Outcomes

| Outcome | Meaning |
|---|---|
| `Translated` | the output messages replace the input |
| `Passthrough` | same symbol and layout in both families; forward the input |
| `Dropped` | consumed; forwarding it would make the game service discard the rest of its packet |
| `Local` | the bridge answers the game client itself (`toGameClient`) |
| `Unsupported` | not a row; the frame functions do not forward it |
| `Malformed` | a row, but the payload is not its layout |

## Rows, by the connection that carries them

The roles follow `evr_codec.h`: the login connection, the config connection, and the matchmaker connection.

### Login connection

| Legacy | Current | Outcome | Layout source | Notes |
|---|---|---|---|---|
| `SNSLoginRequest` | `SNSLogInRequestv2` | Translated | EchoRelay docs (legacy); game service decode (current) | the 8-byte locale has no current slot; the JSON is replaced by `Context::loginProfileJson` when set |
| `SNSLogInSuccess` | `SNSLogInSuccess` | Passthrough | game service decode; EchoRelay docs | |
| `SNSLoginProfileResult`, success byte | `SNSLoggedInUserProfileSuccess` | Translated | EchoRelay docs (legacy); game service decode (current) | zlib `client NUL server` against zstd `{client, server}`; `config` is not carried |
| `SNSLoginProfileResult`, other bytes | `SNSLoginFailure` | Translated | EchoRelay docs (legacy); game service decode (current) | 7 against 401, 8 against 403, anything else against 400; a full 0x30-byte header, no profile |
| `SNSLoginSettings` / `SNSLoginClientSettings` | `SNSLoginSettings` | Passthrough / Translated | EchoRelay docs (legacy); game service decode (current) | the second generation renames the symbol; the payload is identical |
| `SNSRefreshProfile` | `SNSLoggedInUserProfileRequest` (own id) or `SNSOtherUserProfileRequest` | Translated | EchoRelay docs (legacy); game service decode (current) | the request JSON is `Context::profileRequestJson` |
| `SNSProfileRequestv2` / `SNSProfileRequest` | `SNSOtherUserProfileRequest` | Translated | EchoRelay docs (legacy); game service decode (current) | |
| `SNSRefreshProfileResult`, `SNSProfileResponsev2` / `SNSProfileResponse` | `SNSOtherUserProfileSuccess` | Translated | EchoRelay docs (legacy); game service decode (current) | `ProfileReply` says which legacy reply a current answer becomes |
| `SNSUpdateProfile` | `SNSUpdateProfile` | Passthrough | EchoRelay docs (legacy); game service decode (current) | the profile content is not established |
| `SNSLeaderboardRequest` | none | Local | EchoRelay docs; response symbol in our corpus | an empty board under the request's tag |
| `SNSTelemetryEvent`, `SNSMatchEnded`, `SNSMatchEndedv2` | none | Dropped | EchoRelay docs | |

`BuildLoggedInUserProfileRequest` is the message the bridge sends once the login succeeds; a legacy game
client never asks for it.

### Config connection

`SNSConfigRequestv2`, `SNSConfigSuccessv2`, `SNSConfigFailurev2`, `SNSReconcileIAP`, `SNSReconcileIAPResult`
and the transport-level `STcpConnectionUnrequireEvent` pass through unchanged. Source: game service decode
order; `SNSConfigSuccessv2`, `SNSReconcileIAP` and `SNSReconcileIAPResult` are named in our corpus; the legacy
layout of all of them is taken from EchoRelay's documentation and is unverified.

### Matchmaker connection

| Legacy | Current | Layout source | Notes |
|---|---|---|---|
| `SNSLobbyFindSessionRequestv8` | `SNSLobbyFindSessionRequestv11` | EchoRelay docs (legacy); game service decode (current) | login session from `Context`; mode, level and platform through `BuildTables`; two bytes of the legacy form are not carried |
| `SNSLobbyCreateSessionRequestv7` | `SNSLobbyCreateSessionRequestv9` | EchoRelay docs (legacy); game service decode (current) | bytes after the lobby type whose purpose is unknown are not carried |
| `SNSLobbyJoinSessionRequestv6` | `SNSLobbyJoinSessionRequestv7` | EchoRelay docs (legacy); game service decode (current) | the team index becomes the tail; two unknown u64 are not carried |
| `SNSLobbyPlayerSessionsRequestv3` | `SNSLobbyPlayerSessionsRequestv5` | EchoRelay docs (legacy); game service decode (current) | the game client lists itself at the head of its id list, which is where the requesting user is read from |
| `SNSLobbyPendingSessionCancel` | `SNSLobbyPendingSessionCancelv2` | EchoRelay docs (legacy); game service decode (current) | one byte against the login session |
| `SNSLobbySessionSuccessv4` | `SNSLobbySessionSuccessv5` | EchoRelay docs (v4); game service decode (v5); both names in our corpus | the group GUID is the only difference; Quest encoder flags are converted to the PC layout |
| `SNSLobbySessionFailurev3` / `v2` | `SNSLobbySessionFailurev4` | EchoRelay docs (v3, v2); game service decode (v4); all three names in our corpus | v3 drops the message and expiry; v2 also drops the mode and an unknown word |
| `SNSLobbyMatchmakerStatusRequest`, `SNSLobbyMatchmakerStatus`, `SNSLobbyPlayerSessionsSuccessv3` | same | game service decode; names in our corpus | Passthrough |

## Not translated

- the create request's bytes of unknown purpose, and any create request that does not end in the documented
  user id and team;
- `SNSLobbyPingRequestv3` / `SNSLobbyPingResponse`: whether a legacy game client answers is not established;
- `SNSRemoteLogSetv2` against `SNSRemoteLogSetv3`: the v3 header words are not identified;
- `SNSLobbyStatusNotifyv2`, and the older player-session answers `SNSLobbyPlayerSessionsSuccessv0`/`v2`;
- profile, document and channel-info requests a legacy game client may send that are not listed above;
- the earlier generation of lobby builds, whose messages have no packet marker and use find, create and join
  requests with no level or channel (Find v6, v5, v4).

## Verification

`test_legacy_codec` builds every fixture byte by byte with a writer that shares no code with the codec.
Each row is checked legacy to current against a golden, current to legacy against a golden, and round trip.
Fields one family has no slot for are asserted to come back zero. Fixture values are synthetic.
