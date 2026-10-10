# Legacy translator (Summer 2019, Halloween 2018)

`src/runtime/compat/legacy_codec.{h,cpp}` rewrites one message at a time between the message family a legacy
game client speaks and the family the game service (nakama) speaks. The game service only ever sees the
current build's family; the legacy game client only ever sees its own build's.

The translator is a set of pure functions: a result depends only on the symbol, the payload, the `Context`
(login session, own user id, channel, the bridge's login profile JSON) and the `BuildTables` (the symbols
that differ per build family and the build's symbol dictionary). It includes no Windows header and owns no
state, so `src/runtime/tests/test_legacy_codec.cpp` runs it on any host. Nothing in `BugSplat64.dll` calls
it yet.

## Outcomes

| Outcome | Meaning |
|---|---|
| `Translated` | the output messages replace the input |
| `Passthrough` | same symbol and layout on both sides; forward the input |
| `Dropped` | consumed; forwarding it would make the game service discard the rest of its packet |
| `Local` | the bridge answers the game itself (`toGame`) |
| `Unsupported` | not a row of this translator; `TranslateFrame*` does not forward it |
| `Malformed` | a row, but the payload is not its layout |

## Rows

Symbols are named as the game names them. Every constant in `legacy_codec.h` equals the CSymbol64 of its
name; `LegacySymbols.EveryConstantIsTheCSymbol64OfItsGameName` checks that.

### Login

| Legacy | Current | Outcome | Notes |
|---|---|---|---|
| `SNSLoginRequest` | `SNSLogInRequestv2` | Translated | the 8-byte locale is dropped; the JSON is replaced by `Context::loginProfileJson` when set |
| `SNSLogInSuccess` | `SNSLogInSuccess` | Passthrough | |
| `SNSLoginProfileResult` (result 0x0B) | `SNSLoggedInUserProfileSuccess` | Translated | zlib `client\0server` <-> zstd `{client, server}`; `config` is not carried |
| `SNSLoginProfileResult` (other results) | `SNSLoginFailure` | Translated | 7 <-> 401, 8 <-> 403, anything else <-> 400; the result is a 0x30-byte header |
| `SNSLoginSettings` / `SNSLoginClientSettings` | `SNSLoginSettings` | Passthrough / Translated | Halloween 2018 renames; the payload is identical |
| `SNSRefreshProfile` | `SNSLoggedInUserProfileRequest` (own id) or `SNSOtherUserProfileRequest` | Translated | the request JSON comes from `Context::profileRequestJson` |
| `SNSProfileRequestv2` / `SNSProfileRequest` | `SNSOtherUserProfileRequest` | Translated | |
| `SNSRefreshProfileResult` | `SNSOtherUserProfileSuccess` | Translated | `ProfileReply` picks the legacy reply for a current answer |
| `SNSProfileResponsev2` / `SNSProfileResponse` | `SNSOtherUserProfileSuccess` | Translated | |
| `SNSUpdateProfile` | `SNSUpdateProfile` | Passthrough | the client profile content is not established |
| `SNSLeaderboardRequest` | none | Local | answered with an empty board under the request's tag |
| `SNSTelemetryEvent`, `SNSMatchEnded`, `SNSMatchEndedv2` | none | Dropped | |

`BuildLoggedInUserProfileRequest` is the message the bridge sends after the login succeeds; the legacy game
client never asks.

### Configuration

`SNSConfigRequestv2`, `SNSConfigSuccessv2`, `SNSConfigFailurev2`, `SNSReconcileIAP`, `SNSReconcileIAPResult`
and the transport-level `STcpConnectionUnrequireEvent` pass through.

### Matching

| Legacy | Current | Notes |
|---|---|---|
| `SNSLobbyFindSessionRequestv8` | `SNSLobbyFindSessionRequestv11` | the login session comes from `Context`; mode, level and platform go through `BuildTables`; two unknown bytes are not carried |
| `SNSLobbyCreateSessionRequestv7` | `SNSLobbyCreateSessionRequestv9` | the legacy bytes between the lobby type and the channel are not established and are not carried |
| `SNSLobbyJoinSessionRequestv6` | `SNSLobbyJoinSessionRequestv7` | the team index becomes the tail; two unknown u64 are not carried |
| `SNSLobbyPlayerSessionsRequestv3` | `SNSLobbyPlayerSessionsRequestv5` | the requester is the first listed id |
| `SNSLobbyPendingSessionCancel` | `SNSLobbyPendingSessionCancelv2` | one byte <-> the login session |
| `SNSLobbySessionSuccessv4` | `SNSLobbySessionSuccessv5` | the group GUID is the only difference; Quest encoder flags are converted to the PC layout |
| `SNSLobbySessionFailurev3` / `v2` | `SNSLobbySessionFailurev4` | v3 drops the message and expiry; v2 also drops the mode and the unknown word |
| `SNSLobbyMatchmakerStatusRequest`, `SNSLobbyMatchmakerStatus`, `SNSLobbyPlayerSessionsSuccessv3` | same | Passthrough |

## Not translated

- the Create v7 unmapped bytes, and any capture that ends elsewhere than the documented user id and team;
- `SNSLobbyPingRequestv3` / `SNSLobbyPingResponse`: whether a legacy game client answers is not established;
- `SNSRemoteLogSetv2` -> `SNSRemoteLogSetv3`: the v3 header words are not named;
- `SNSLobbyStatusNotifyv2`, the legacy `SNSLobbyPlayerSessionsSuccessv2`;
- document, channel-info and other-user rows a legacy game client may send: not established;
- the 2017 family (headerless framing, Find v6/v5/v4).

## Verification

`test_legacy_codec` builds every fixture from the documented layouts with an independent byte writer.
Each row is checked legacy -> current against a golden, current -> legacy against a golden, and round trip.
Fields one side has no slot for are asserted dropped (they come back zero).
