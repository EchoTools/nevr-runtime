# The daily scenario smoke

`nevr-scenario-smoke.timer` starts `nevr-scenario-smoke.service` at 04:00 daily. The service runs
`tools/scenario/smoke.sh` from a worktree of its own, `.claude/worktrees/nevr-scenario-smoke`, which
tracks `origin/main`; no one develops in it. `smoke.sh` fetches, fast-forwards that worktree to
`origin/main`, re-runs itself from the updated file, builds the `mingw-scenario` DLL, boots one client
on `:101` and checks that it reaches in game, that the control endpoint listens and that a fire drives
the game. It fails with the alarm (bonk and a courier to spritz) when it cannot fast-forward, when the
build fails or when a check fails.

The record is `/var/tmp/work-nevr-runtime/scenario-smoke/last.json` and `last.txt`; the run's own
`result.json`, `console.log` and `game.jsonl` are in `.../scenario-smoke/run/`. `result.json` and
`last.json` carry `loadavg_start` (`/proc/loadavg` when the run started) and `states` (every NetGame
state change with its timestamp). A pass that reached in game after more than 300 s is a PASS with
`"slow": true`; the in-game wait is 600 s.

## One-time setup

```sh
git fetch origin main
git worktree add --no-track -b nevr-runtime/792-smoke-main .claude/worktrees/nevr-scenario-smoke origin/main
git worktree lock --reason "daily smoke timer runs from here; do not develop in it" .claude/worktrees/nevr-scenario-smoke
(cd .claude/worktrees/nevr-scenario-smoke && just worktree-setup)
mkdir -p ~/.config/systemd/user
ln -sf "$PWD/.claude/worktrees/nevr-scenario-smoke/tools/scenario/systemd/nevr-scenario-smoke.service" ~/.config/systemd/user/
ln -sf "$PWD/.claude/worktrees/nevr-scenario-smoke/tools/scenario/systemd/nevr-scenario-smoke.timer" ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now nevr-scenario-smoke.timer
```

`just smoke` runs the same script by hand; `smoke.sh --no-sync` skips the fast-forward and
`--sync-only` stops after it.
