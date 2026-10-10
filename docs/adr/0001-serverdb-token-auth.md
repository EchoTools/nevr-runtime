# ADR 0001: Game-server ServerDB registration authenticates with a JWT

Status: accepted, live.

## Context

A game server registered with ServerDB over a WebSocket whose URL carried
`discord_id` and `password` as query parameters. The same route
(`nevr_socket_uri`, `/spr`) is the client login bridge, and nginx injects the
server key on it, so a caller's own `Authorization` header never reaches Nakama.

## Decision

- Registration dials its own route, `/nevr`, configured by its own key
  `nevr_serverdb_uri` (`services.serverdb_uri`). nginx forwards the caller's real
  `Authorization: Bearer` on `/nevr` (an additive `location ^~ /nevr`). Every
  other route is unchanged.
- The server acquires a session JWT through the non-interactive password RPC
  (`POST {nevr_http_uri}/v2/rpc/account/authenticate/password?http_key=<key>&unwrap`,
  body `{"discord_id","password"}`) and sends it as the Bearer on the upgrade.
  The token TTL is about an hour, so it is re-acquired on every registration.
  `src/runtime/server/gameserver_serverdb.cpp` implements this.
- `nevr_serverdb_uri` and `nevr_socket_uri` stay separate keys. Pointing
  `nevr_socket_uri` at `/nevr` breaks client login, which still authenticates with
  `discord_id` and `password` on `/spr`.
- Token auth is opt-in. When `nevr_serverdb_uri` is unset the server falls back to
  `nevr_socket_uri` with URL-parameter credentials.

Acceptance (all hold on prod): login via `/spr` succeeds; registration acquires a
JWT, connects to `/nevr`, and stays connected with no disconnects; `/nevr` with a
valid JWT returns 101; `/nevr` with a garbage token returns 401.

## Consequences

- A deploy must set `nevr_serverdb_uri` (for example
  `ws://g.echovrce.com:80/nevr`). Token-only registration over `/spr` fails, and
  the fallback to `nevr_socket_uri` hides that.
- The nginx change lives on the production host and is not tracked in an ops repo.
- The client login bridge still uses `discord_id` and `password` on `/spr`.
  Moving it to a token touches every client and is a separate decision.
