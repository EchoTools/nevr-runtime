# ServerDB auth: URL-param → JWT (as built, 2026-06-29)

> **Record, not a procedure.** This documents what was built and is live on
> prod. The original ADR body — which proposed a `/sprt` route and reusing
> `nevr_socket_uri` — was removed on 2026-07-27: `/sprt` was rejected in favour
> of `/nevr`, and the doc's own text records that pointing `nevr_socket_uri` at
> the token route **broke login**. Anyone following those sections would have
> implemented the documented failure. Full text in git history.

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
