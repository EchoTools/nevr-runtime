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

`just nakama-seed` inserts a test account and a guild group it belongs to
(`tools/nakama-local/seed.py`: fake Discord ID `900000000000000001`, throwaway password,
guild `900000000000000002`; `--print` shows the runtime `identity:` block). It uses SQL
because the fork disables the email and device authenticate APIs. Login refuses a user
in no guild group ("user is not in any groups", `server/evr_pipeline_login.go`), and the
guild registry re-reads groups once a minute, so allow up to 60 s after the first seed.

## Logging in from the Windows VM

`WINVM_USER=… WINVM_PASS=… tools/winvm/systest.py --scenario login` writes a runtime
`config.yaml` for the rig (`services.socket_uri` = `ws://192.168.122.1:7350/ws?format=evr&token=<server_key>`,
the seeded identity), boots the server headless, and judges nakama's own log
(`checks.check_nakama_login`): websocket auth, then `LoginSuccess` vs `LoginFailure`.

Verified 2026-09-21: nakama sent `LoginSuccess`, the runtime logged `[NEVR.WS] LOGIN SUCCESS`
and fetched the profile. Falsified: the same run before the guild group existed failed with
`LoginFailure: user is not in any groups`; with nakama unreachable it failed with no session.

Gotchas: the token must be in `socket_uri` (the bridge does not add it; production's proxy
does); the rig's offline `config.json` hosts (`127.0.0.1:1`) override the bridge redirect,
so this scenario writes a `config.json` without `*_host` keys; docker's port proxy shows
every client as the bridge gateway, so nakama's `client_ip` cannot identify the VM.

## Not covered yet

- Server registration (`regions=` / guild registration) and the matchmaker path.
- Which User-Agent each client sends (nakama #629).
