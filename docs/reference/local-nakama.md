# Local nakama

A throwaway EchoTools nakama for reproducing login, registration and voice/connection
bugs without a live tester or the production server.

```sh
just nakama-up      # generates .state/ on first run, then starts the stack
just nakama-logs
just nakama-down    # keeps the database;  just nakama-reset  drops it
```

Reachable at `127.0.0.1:7350` (API/sockets) and `127.0.0.1:7351` (console), and at
`192.168.122.1:7350` for the Windows VM (`docs/reference/windows-vm-system-test.md`).
It never binds the LAN. Console credentials are in `tools/nakama-local/.state/nakama.yml`.

## What is in the box

`tools/nakama-local/docker-compose.yml`: Postgres 17, the same nakama image production
runs (`ghcr.io/echotools/nakama:v3.27.2-evr.325`, which `:latest` resolved to on
2026-09-21), and `discord_mock.py`. Keys and a throwaway CA are generated into
`tools/nakama-local/.state/` (git-ignored) by `tools/nakama-local/setup.py`; nothing in
git is a secret, and nothing here touches production data.

## Why there is a fake Discord

The fork will not start without a live Discord gateway: `server/evr_pipeline.go` calls
discordgo's `Open()` and then waits 10 s for `READY`, fatally, with no offline switch.
A real bot token is not an option for a local test (it would register slash commands and
sync members under a real application), so `discord_mock.py` answers just enough of
Discord under its real hostnames (network aliases `discord.com`, `gateway.discord.gg`):
the gateway handshake, `gateway`/`users/@me`/`applications/@me`, empty command sync, and
Discord's own "Unknown Member/Guild/User" and "cannot DM" error codes for lookups, which
the login path already handles. nakama trusts the mock's CA through `SSL_CERT_FILE`.
That variable replaces the system roots on purpose: nothing else should be reachable.

## Startup requirements found the hard way

- `DISCORD_BOT_TOKEN` must be set (fake is fine), or the EVR runtime module fails init.
- `IP_ADDRESS_INTERNAL` / `IP_ADDRESS_EXTERNAL` must be set, or startup asks
  `api.ipify.org` and dies without an answer.
- Optional services (VRML OAuth, ASN data download) log warnings and are otherwise skipped.

## Seeding a login

`just nakama-seed` inserts a test account (`tools/nakama-local/seed.py`: fake Discord ID
`900000000000000001`, throwaway password; `--print` shows the runtime `identity:` block).
It uses SQL because the fork disables the email and device authenticate APIs.
Verified: the row exists with `custom_id` set and a bcrypt password.

## Not covered yet

- Verified 2026-09-21: `/ws?format=evr&discordid=…&password=…&token=<server_key>` upgrades
  (`session_ws.go:133` "New WebSocket session connected"); the server then waits for a
  LoginRequest and closes idle sockets after ~3 s with no auth error logged. Without
  `token=` the upgrade is a 401 (`server/socket_ws.go`). Not yet proven: a LoginRequest
  is accepted and LoginSuccess returned (needs the runtime, or a hand-built frame).
  nakama logs the full query string, password included: fine here, never copy prod logs.
- A guild group the account belongs to (server registration reads the guild groups), and
  the matching runtime config (`auth.socket_uri` pointing at `ws://192.168.122.1:7350/ws`,
  `identity.*`, `auth.server_key`), are not seeded or wired into `tools/winvm/systest.py`.
