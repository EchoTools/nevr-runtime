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

Two kinds of source, and only the second has been checked against anything of ours:

- **Legacy side.** Byte layouts, field order and the login result values were learned by reading EchoRelay's
  message documentation and test fixtures. They are wire facts only, written fresh here. **None has been
  verified against a legacy binary or a live packet.** When the legacy exes arrive, every legacy-side row is
  to be checked against them.
- **Current side.** The decode order of the game service's codec (nakama, `server/evr/*.go`), read-only. These
  rows are what the game service accepts today; `evr_codec.h` confirms the login-connection symbols it names.
- **Our corpus.** `src/runtime/hook/symbol_corpus.cpp` and `src/runtime/log/symcache_data.cpp` come from the
  final build's binary. They contain these names, so the names and symbols below are confirmed as game
  symbols: `SNSLobbySessionSuccessv4`, `SNSLobbySessionSuccessv5`, `SNSLobbySessionFailurev2`, `v3`, `v4`,
  `SNSLobbyMatchmakerStatus`, `SNSLobbyPlayerSessionsSuccessv3`, `SNSConfigSuccessv2`, `SNSReconcileIAP`,
  `SNSReconcileIAPResult`, `SNSLeaderboardResponse`. The other legacy symbols are not in the final build
  (they were removed with their messages); their values are the CSymbol64 of the name, which the tests check,
  but the name itself comes from EchoRelay.

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

| Legacy | Current | Outcome | Notes |
|---|---|---|---|
| `SNSLoginRequest` | `SNSLogInRequestv2` | Translated | the 8-byte locale has no current slot; the JSON is replaced by `Context::loginProfileJson` when set |
| `SNSLogInSuccess` | `SNSLogInSuccess` | Passthrough | |
| `SNSLoginProfileResult`, success byte | `SNSLoggedInUserProfileSuccess` | Translated | zlib `client NUL server` against zstd `{client, server}`; `config` is not carried |
| `SNSLoginProfileResult`, other bytes | `SNSLoginFailure` | Translated | 7 against 401, 8 against 403, anything else against 400; a full 0x30-byte header, no profile |
| `SNSLoginSettings` / `SNSLoginClientSettings` | `SNSLoginSettings` | Passthrough / Translated | the second generation renames the symbol; the payload is identical |
| `SNSRefreshProfile` | `SNSLoggedInUserProfileRequest` (own id) or `SNSOtherUserProfileRequest` | Translated | the request JSON is `Context::profileRequestJson` |
| `SNSProfileRequestv2` / `SNSProfileRequest` | `SNSOtherUserProfileRequest` | Translated | |
| `SNSRefreshProfileResult`, `SNSProfileResponsev2` / `SNSProfileResponse` | `SNSOtherUserProfileSuccess` | Translated | `ProfileReply` says which legacy reply a current answer becomes |
| `SNSUpdateProfile` | `SNSUpdateProfile` | Passthrough | the profile content is not established |
| `SNSLeaderboardRequest` | none | Local | an empty board under the request's tag |
| `SNSTelemetryEvent`, `SNSMatchEnded`, `SNSMatchEndedv2` | none | Dropped | |

`BuildLoggedInUserProfileRequest` is the message the bridge sends once the login succeeds; a legacy game
client never asks for it.

### Config connection

`SNSConfigRequestv2`, `SNSConfigSuccessv2`, `SNSConfigFailurev2`, `SNSReconcileIAP`, `SNSReconcileIAPResult`
and the transport-level `STcpConnectionUnrequireEvent` pass through unchanged.

### Matchmaker connection

| Legacy | Current | Notes |
|---|---|---|
| `SNSLobbyFindSessionRequestv8` | `SNSLobbyFindSessionRequestv11` | login session from `Context`; mode, level and platform through `BuildTables`; two bytes of the legacy form are not carried |
| `SNSLobbyCreateSessionRequestv7` | `SNSLobbyCreateSessionRequestv9` | the run of bytes between the lobby type and the channel is not identified and not carried |
| `SNSLobbyJoinSessionRequestv6` | `SNSLobbyJoinSessionRequestv7` | the team index becomes the tail; two unknown u64 are not carried |
| `SNSLobbyPlayerSessionsRequestv3` | `SNSLobbyPlayerSessionsRequestv5` | the requester is the first listed id |
| `SNSLobbyPendingSessionCancel` | `SNSLobbyPendingSessionCancelv2` | one byte against the login session |
| `SNSLobbySessionSuccessv4` | `SNSLobbySessionSuccessv5` | the group GUID is the only difference; Quest encoder flags are converted to the PC layout |
| `SNSLobbySessionFailurev3` / `v2` | `SNSLobbySessionFailurev4` | v3 drops the message and expiry; v2 also drops the mode and an unknown word |
| `SNSLobbyMatchmakerStatusRequest`, `SNSLobbyMatchmakerStatus`, `SNSLobbyPlayerSessionsSuccessv3` | same | Passthrough |

## Not translated

- the unidentified bytes of the create request, and any create request that does not end in the documented
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
