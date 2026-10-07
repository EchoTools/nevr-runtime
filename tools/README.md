# tools/

Scripts the build, the gate and the tests use. `just verify` runs the `verify_*.py` checks and the
unit tests in `tests/`; the rest are run by hand or by a `just` recipe.

## Gate checks (run by `just verify`)

| Script | What it checks |
| --- | --- |
| `verify_doc_paths.py` | Every repo path a current-state document claims resolves, and every `git show <sha>:<path>` citation resolves. |
| `verify_hook_invariants.py` | Static hook invariants: no address is both called through a live function pointer and detoured, hooked prologues match `hook_identity_manifest.json`, no detour that a plugin also installs. |
| `verify_log_rules.py` | The mechanically checkable rules of `docs/standards/logging.md` (for example, no INFO line in a per-frame path). |
| `verify_mode_patch_ground_truth.py` | Every plain byte rewrite in `mode_patches.cpp` has binary ground truth in `tests/system/mode_patches_test.go`. |
| `verify_patch_source_inventory.py` | The `patch/*.cpp` entries of `PATCHES_SOURCES` match the expected inventory (update it deliberately when a patch is added or removed). |
| `verify_scenario_control_absent.py` | A release `BugSplat64.dll` carries no scenario-test control endpoint. |

`tests/` holds the Python unit tests for these scripts and for the shell and Python tools below
(`python3 -m unittest discover -s tools/tests -p 'test_*.py'`).

## Run a game or the device

| Script | What it does |
| --- | --- |
| `../launch-client.sh` (repo root) | Client login test on the nested display; see AGENTS.md. Sources `lib/game_install.sh`. |
| `../verify-server.sh` (repo root) | Instrumented dedicated-server run that restores what it deployed. Sources `lib/game_install.sh`. |
| `lib/game_install.sh` | Shared by those two: game-root resolution, the one-run-at-a-time lock, the running-game check. |
| `quest-install.sh` | Sideload the repacked Quest APK and the game data (run by `just quest-install`). |
| `scenario/` | One-client social scenarios and their runner (`just scenario NAME`). |
| `winvm/` | Run the built runtime on a native Windows VM; see `winvm/README.md`. |

## Generators and data

| Item | Purpose |
| --- | --- |
| `build_distribution.py` | Sign a staged runtime package and atomically publish verified archives. |
| `gen_symbol_corpus.py`, `symbol_candidates.txt` | Hash candidate symbol names and emit the reverse-lookup tables (CSymbol64 and SNS message hashes). |
| `generate-symcache.sh` | Regenerate `src/runtime/log/symcache_data.cpp` from the evrcat symbol cache. |
| `hook_identity_manifest.json` | The pinned prologues `verify_hook_invariants.py` checks. |
| `echomod/` | Standalone Echo VR modding scripts; see `echomod/README.md`. |
