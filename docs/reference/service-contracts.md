# Service contracts

Wire shapes between the runtime and the game service (nakama) or a site, for features whose runtime code is not written
yet. Each row is a Go file in `tests/contracts/` (`just test-service-contracts`): a strict parser or a pure decision, and a
test that pins it. A service checks itself against the same vectors. The service half of each is an issue in the service's
own repo; the runtime half follows the contract.

| Issue | Contract | File | Service half |
| --- | --- | --- | --- |
| #62 | Player report: reported evr id, match id, reason, UTC time. No reporter field: the service takes it from the session. Several reporters of one player raise a moderator marker. | `report.go` | nakama (a message handler, the log, the marker) |
| #446 | Slow-load notice: the service owns the trigger (`LOAD_STATS` `client_load_time` over 30 s) and the per-player history, so it sends; the runtime shows it. Once per N sessions, never per round. The notice links only to the latest selfsign-path-tool release. | `slowload.go` | nakama |
| #963 | Already defined: `src/runtime/patch/guild_picker.h` and its test; the service half is nakama #720. | (C++ header) | nakama #720 |
| #1536 | Registration sends identity and measured facts; `guilds` and `regions` are optional overrides. The answer assigns guilds and regions (either list may be empty). | `registration.go` | nakama |
| #1689 | Already defined and built: `src/core/crash_send.h` with its allowlist test, ADR 0002 and the nevr-api route `POST /v3/crash-reports`. | (C++ header) | echovrce-web, exists |
| #1726 | Share link `https://echo.taxi/<code>`: the code's alphabet (31 symbols), case-insensitive, hyphens ignored, at most 63 characters. | `sharelink.go` | echotaxi (the site) |
| #1788 | Already defined: ADR 0030 and `src/core/log_send.h`. | (ADR) | echovrce-web: `POST /v3/player-logs`, `GET /v3/player-logs/{code}` |
| #1859 | Lobby kinds `combat` (default), `arena`, and a mature variant of each. Any lobby queues for any mode. An unknown kind is the default. On Quest an arena lobby loads no combat section. | `lobby.go` | nakama |
| #1875 | Friend presence decides the button: in game, the blue +; Discord only, the other color, and the press sends a code by Discord message; offline, no button. | `friendinvite.go` | nakama |
| #1987 | The create-game menu stays closed while a suspension is active; an unreadable end keeps it closed. | `suspension.go` | nakama #765 (the server refuses a create) |
| #2356 | `nevr_cargo` in the login settings and `nevr_install` in the login profile. The row for the lobby's guild wins over the session's choice, which wins over the default. A manifest mounts only when its sha256 is one nakama named or the default's. | `cargo.go` | nakama |
