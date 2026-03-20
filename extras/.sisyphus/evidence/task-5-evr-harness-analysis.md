# evr-test-harness server mode startup analysis

## Files reviewed
- `/home/andrew/src/evr-test-harness/internal/echovr/process.go`
- `/home/andrew/src/evr-test-harness/internal/echovr/session.go`
- `/home/andrew/src/evr-test-harness/internal/echovr/config.go`

## What `evr-mcp_echovr_start` executes
`SessionManager.Start` calls `StartProcess(ctx, httpPort, gametype, moderator, spectator, level)`.

`StartProcess` builds and executes:

```
wine ./ready-at-dawn-echo-arena/bin/win10/echovr.exe \
  -noovr \
  -windowed \
  -httpport <port> \
  -gametype <gametype> \
  -mp \
  [-moderator] \
  [-spectatorstream] \
  [-level <level>]
```

Defaults (from `SessionManager.Start`):
- `gametype`: `echo_arena`
- `httpPort`: `6721`
- `level`: empty unless provided in input

Headless env (if `EVR_HEADLESS` not set to false/0):
- `DISPLAY=:<xvfb>` (Xvfb started on :90 + offset)
- `WINEDEBUG=-all`
- `AUDIODEV=null`
- `SDL_AUDIODRIVER=dummy`

No other flags are passed.

## Why this results in client mode
The harness always launches the standard game executable with client-style flags only:
- It **does not** pass any explicit dedicated/server flags (no `-server`, `-dedicated`, etc.).
- It **does not** switch or inject any server config by default; `Start` does not call config manager functions.
- The only mode-related flags are `-mp`, `-gametype`, and optional `-level`, which are consistent with client matchmaking behavior.

As a result, the process starts as a normal client session even when `-mp` and `-level` are provided. There is nothing in `evr-test-harness` startup logic that triggers a server-only path.

## Implications for server-mode startup
If the game requires a dedicated/server-specific flag or config (outside of `-mp` and `-level`), `evr-mcp_echovr_start` never supplies it. The harness currently has **no parameter or branch** to toggle a server-mode startup path.

## Suggested next checks (outside this task)
- Identify the actual server-mode activation criteria in the game/patch code (flag/config/level requirement).
- Extend `StartProcess` to include that criteria when requested (e.g., new input field).
