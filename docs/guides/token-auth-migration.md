# Game-Server ServerDB Auth: URL-param → Token (JWT)

Status: **registration path implemented + verified E2E on prod** (2026-06-29).
Author: Spritz.

## As built (2026-06-29)

- **nginx `/nevr` route: LIVE on prod** (fortytwo, Andrew-approved). Additive
  `location ^~ /nevr` that forwards the caller's real `Authorization: Bearer`
  (vs the `/spr` catch-all which injects the server key). Regression-checked:
  every other route unchanged. `default.conf.bak.tokenauth` is the pre-change backup.
- **Route name is `/nevr`** (not `/sprt`).
- **Registration uses its own config key `nevr_serverdb_uri`** (→ `ws://…/nevr`),
  kept distinct from `nevr_socket_uri` (→ `/spr`, the client/login bridge, which
  is unchanged and still uses `discord_id`+`password`). Pointing the shared
  `nevr_socket_uri` at `/nevr` broke login — that's why the keys are split.
- **Verified E2E** (`tests/token-auth-smoke.sh`, all BACs green): login via `/spr`
  succeeds, registration acquires a JWT + connects `/nevr` + stays connected
  (0 disconnects, full window). `/nevr`+valid JWT→101, `/nevr`+garbage→401.

### Remaining
- **Deploy to prod game servers** (chi1 etc.): set `nevr_serverdb_uri=ws://g.echovrce.com:80/nevr`
  and deploy the new DLL. This is a prod deploy — Andrew's call. The runtime falls
  back to `nevr_socket_uri` if `nevr_serverdb_uri` is unset, so a deploy MUST set it
  (token-only registration over `/spr` fails — F4).
- **Track the nginx change in the ops repo** for audit/repeatability.
- **Client/login bridge → token** (Path B): still `discord_id`+`password` on `/spr`.
  Moving it to token touches *all* clients (high blast radius) — a separate,
  deliberate step.

## Context

A dedicated game server (`nevr-runtime`, deployed as `BugSplat64.dll`) authenticates
to ServerDB (Nakama, behind nginx on `g.echovrce.com` = fortytwo, 45.56.101.202) over
a WebSocket. Today it passes the operator's credentials as **URL query params**:

```
ws://g.echovrce.com:80/spr?discord_id=<id>&password=<pw>&guilds=<g>&regions=<r>
```

Goal: move server auth to a **session JWT (token auth)**, dropping the long-lived
password from the URL.

## Verified facts (observed, not inferred)

- **F1 — token acquisition works.** `POST {nevr_http_uri}/v2/rpc/account/authenticate/password?http_key=<key>&unwrap`
  with body `{"discord_id","password"}` returns `{"token","refresh_token"}`.
  Verified live 2026-06-29: access JWT `uid=5c5a6d66-9e88-4919-a84e-6f5621deb8a2`
  (`usn=metis.sprock` — the operator account that carries the server-host role),
  `vrs` has no `refresh` flag (it is an access token), observed TTL ≈ **1h**.
  Handler: `nakama server/evr_runtime_rpc.go:1137` (`AuthenticatePasswordRPC`,
  `RequireAuth:false`); registered `server/evr_runtime_rpc_registration.go:34`.
- **F2 — nginx overwrites the Authorization header with the server key.** The WS
  catch-all `location ~* ^/(.*)` in `deployment/nginx_conf.d/default.conf` does
  `proxy_set_header authorization "Bearer 65675147-..."` (the server key) on every
  WS connection, then `proxy_pass http://nakama:7350/ws?format=evr$args`. `/spr` is
  not a named route — it falls through to this catch-all.
- **F3 — the acceptor uses that header → anonymous session.** Server key as the
  Bearer makes Nakama's WS acceptor treat the session as anonymous (`userID` nil);
  identity is then established in-band from `discord_id`+`password`
  (`nakama server/session_ws.go:227-293`). Registration authorizes against
  `session.userID` via `gg.IsServerHost` (`server/evr_pipeline_gameserver.go:246,303`).
- **F4 — token-only over `/spr` fails (the reason this matters).** Sending the JWT
  (header or `?token=`) over `/spr` and dropping `discord_id`+`password` was tested
  end-to-end 2026-06-29: the server **registered then closed in 41 ms** (1005), i.e.
  the registration auth gate failed because the session was anonymous (the injected
  server-key header wins over `?token=`; the JWT is ignored).
- **F5 — query string is forwarded.** The catch-all appends `$args` to
  `/ws?format=evr`, so query params reach Nakama (this is why the legacy
  `discord_id`+`password` work). The header injection, not param-stripping, is the
  blocker.
- **F6 — direct `:7350/ws` is not a usable bypass.** Bare → 401; with a valid JWT →
  400 (it needs the context the catch-all injects). Bypassing the public ingress is
  also the wrong architecture.

## Decision (ADR)

**Add a dedicated, token-authenticated WebSocket ingress route on nginx that forwards
the client's real `Authorization: Bearer <jwt>` instead of injecting the server key.
Point dedicated servers at it. Leave the `/spr` catch-all and every other route
untouched.**

- New route (proposed path `/sprt`; exact name is free — "doesn't need to be /spr"):
  same WS handling as the catch-all **except** `proxy_set_header authorization
  $http_authorization;` (forward the caller's JWT) with no server-key injection.
- More-specific `location ^~ /sprt` takes precedence over the `~* ^/(.*)` catch-all,
  so existing clients on `/spr` are unaffected — additive change, no regression
  surface on other routes.

### Alternatives rejected
- **Modify the `/spr` catch-all** to forward the header: would change auth for *all*
  WS clients (the catch-all serves everything) — high blast radius. Rejected.
- **`?token=` over `/spr`:** refuted (F4 — injected server-key header wins).
- **Bypass to direct `:7350/ws`:** refuted (F6).

## Behavioral Acceptance Criteria

- **BAC-1 (token acquisition):** Given valid `nevr_discord_id`+`nevr_password`+`nevr_http_key`,
  `AuthenticateServer()` returns a non-empty access JWT whose decoded `uid` equals the
  operator account. (Test: `tests/system` auth test against the RPC; asserts token + `uid`.)
- **BAC-2 (token transport):** The dedicated server connects to the token route with
  the JWT as `Authorization: Bearer`, and **no** `discord_id`/`password` in the URL.
- **BAC-3 (authenticated registration):** A server connecting via the token route with
  a valid operator JWT reaches `GameServerRegistration` with `session.userID` = operator
  and **stays connected** past the healthcheck window (no 1005 auth-close).
- **BAC-4 (no regression on other routes):** After adding the nginx route, the existing
  `/spr` client path, the `/v2/*`, `/status/*`, `/player/*`, `/oauth/*`, `/grafana`,
  and the `echovrce.com` SPA routes all still serve correctly (status + sample bodies).
- **BAC-5 (re-auth):** Because the access token TTL ≈ 1h, the server re-authenticates
  (re-POST the password RPC) on (re)registration; a stale/expired token never silently
  produces an anonymous session.

## Changes

### Runtime (`src/gamepatches/gameserver/gameserver.cpp`)
1. `AuthenticateServer()` → password RPC (F1). [verified]
2. `RequestRegistration()`: connect to `nevr_socket_uri` (the token route) with the JWT
   as the Bearer (via `m_wsClient->Connect(uri, jwt)`); drop `discord_id`/`password`
   from the URL; keep `guilds`/`regions` as registration metadata.
3. Config: `nevr_socket_uri` → the token route (e.g. `ws://g.echovrce.com/sprt`).

### nginx (`deployment/nginx_conf.d/default.conf`) — additive, needs prod approval
New `location ^~ /sprt` block: copy of the WS catch-all, with
`proxy_set_header authorization $http_authorization;` (no server-key injection).

## Test plan

- **Automated (BAC-1):** system test hitting the password RPC, asserting a valid
  operator JWT. Reusable Go tool, committed.
- **End-to-end (BAC-2/3):** run the dedicated server (Wine, UPnP-reachable) against
  the token route; assert `Registered lobby` + zero `ServerDB disconnected` over a
  ≥60s window (vs the 41ms 1005 close on the old path).
- **Regression (BAC-4):** before/after the nginx route, probe every other location in
  `default.conf` (status codes + sample bodies) and confirm unchanged. `nginx -t`
  must pass before any reload.

## Rollout

1. Land + verify the runtime change locally.
2. Present the nginx `location` diff for Andrew's explicit approval.
3. Apply on fortytwo: edit `default.conf`, `nginx -t`, reload the nginx container.
4. Run BAC-3 + BAC-4 against prod; only then flip the deployed config's
   `nevr_socket_uri` to the token route.
