---
name: run-on-a-copy
description: Use before any launch-client.sh, launch-server.sh, verify-server.sh, scenario run or Go system suite. Makes the run's own copy of the game install from the pristine copy and refuses it unless it is safe to run from.
---

# Run on a copy

The procedure for "The game install" in `AGENTS.md`. The rules there are the rules; this is the order.

## Steps

1. Find the pristine copy (`<work root>/<seat>/game-copy`). If none exists, stop that run and report that a human must
   make it ("Who makes it, and reuse"); go on with other work.
2. `df -h /var/tmp`: free space must exceed twice `du -sh` of the install.
3. Make the run's own copy from the pristine one, under your scratch directory, as `AGENTS.md` "The game install", "Who makes it, and reuse", says.
4. In the run copy move out `_local/config.yaml`, `_local/config.json`, every `plugins` directory's files and every
   `.credentials.json*`, as the AGENTS.md "Make it safe" block says.
5. Run `go -C tools run ./cmd/run-copy -copy <run copy> -src <the install>`. Exit 1 prints each refusal: fix it, run again.
   The `leaves-copy:` lines are the Wine prefix's device and home links; keep them in the run's record.
6. Set `NEVR_GAME_ROOT=<run copy>`, `EVR_GAME_DIR=<run copy>/echovr`, `NEVR_RUN_SCRATCH_ROOT=<scratch>`; run in the slice
   and with the display variables `AGENTS.md` names.
7. When the run ends, remove the run copy by its exact path (the scratch rule).

## Artifact and consumer

The run copy and `run-copy`'s output. Consumer: `tools/cmd/run-copy`, which refuses the copy; the launch scripts
(`refuse_copy_credentials`, exit 3) refuse sign-in files on their own.

## Done when

`run-copy` exits 0, and the run's record names the copy path and the `leaves-copy:` lines.

## Proof of the check

`go -C tools test ./cmd/run-copy`: a sign-in file (also in the nevr root of the Wine prefix), a config file (also the nevr root's `config.yaml`), a
plugin, a missing `echovr.exe`, a link into the install, a missing `-src`, an unreadable directory and a copy outside
the work root are each refused; a clean copy passes.
