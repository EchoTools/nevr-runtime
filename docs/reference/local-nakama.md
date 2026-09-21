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

## Not covered yet

A running server is not a working login. To log a runtime in, the instance still needs a
seeded account (Discord ID + password), a guild group it belongs to, and a matching
runtime config (`services.socket_uri`, `_local/config.json`). None of that is seeded yet.
