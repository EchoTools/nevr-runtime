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
| `verify_scenario_control_absent.py` | A release `BugSplat64.dll` and `nevr-runtime.dll` carry no scenario-test control endpoint. |
| `check_fresh_test_programs.py` | The test programs `just test-scripting` runs are declared by the build it ran and on disk: a stale or undeclared program fails the run (run by `test-scripting` after each build). |

`tests/` holds the Python unit tests for these scripts and for the shell and Python tools below, plus
source-invariant tests over the C++ tree and the release contract
(`python3 -m unittest discover -s tools/tests -p 'test_*.py'`).

## Run a game or the device

| Script | What it does |
| --- | --- |
| `../launch-client.sh` (repo root) | Client login test on the nested display; see AGENTS.md. Sources `lib/game_install.sh`. |
| `../verify-server.sh` (repo root) | Instrumented dedicated-server run that restores what it deployed. Sources `lib/game_install.sh`. |
| `lib/game_install.sh` | Shared by those two: game-root resolution, the one-run-at-a-time lock, the running-game check. |
| `quest-install.sh` | Front end over `install-nevr-runtime quest install`: installs the signed release's APK and game data, or a local tester APK with `--artifact-dir` (run by `just quest-install`). |
| `quest_store_build.py` | The bookkeeping of `just quest-store-build` (ADR 0042): `require` stops, naming the variable, unless `NEVR_STORE_APK`, `NEVR_STORE_OBB` and the signing keystore are usable; `check` is the cache check (exit 0 hit, 3 miss, naming the input that changed); `collect` links the signed APK and the stock `_data` into `build/quest-store`, checks them against the pins and writes `store-build.json`. One JSON line per step. |
| `quest_store_install.py` | The headset step of the store build (`just quest-store-install`): `adb install -r`, then the stock `_data` is pushed to a staging folder, hashed on the device and moved over `/sdcard/Android/media/com.readyatdawn.r15/files/_data`. Refuses without exactly one ready device or when `build/quest-store` differs from its manifest; uninstalls nothing. |
| `scenario/` | One-client social scenarios and their runner (`just scenario NAME`). |
| `winvm/` | Run the built runtime on a native Windows VM; see `winvm/README.md`. |

## Generators and data

| Item | Purpose |
| --- | --- |
| `build_distribution.py` | Sign a staged runtime package and atomically publish verified archives. |
| `gen_symbol_corpus.py`, `symbol_candidates.txt` | Hash candidate symbol names and emit the reverse-lookup tables (CSymbol64 and SNS message hashes). |
| `symcache/` | Go tool that fetches the pinned `Dualgame/echo-symbols` `echo_vr-pc` and `echo_vr-quest` subsets (`tools/symbols.pin`), merges them (PC name wins a case-only collision), validates, and regenerates `src/runtime/log/symcache_data.cpp` deterministically (`just generate-symcache`). |
| `render_credits.py` | Render the README's "Built on, and thanks" block from `config/credits.json`, the one list the in-game CREDITS page also reads. `--check` fails when the block is not the rendering, `--write` regenerates it; `ingame_groups` is how a heading is chosen for the in-game half. `--ingame-source` and `--ingame-text` render that half and refuse, naming the entry and the field, any name or heading the game's font cannot draw (it draws printable ASCII and `©`; `ë` draws as `?`), a line over 64 bytes and a block over 200 lines. The file's `notes` list holds the rules for whoever edits it (never rendered); an unknown key is refused; a source is a github.com URL, an issue number, `named by the project owner`, or one of three exact texts, `echoApkPatch credit strings`, `tested in #nevr-runtime-testing (Discord)` and `echovrce.com website`. |
| `install_game_ndk.py`, `game_ndk.pin` | Install the game's NDK (r21b), pinned by the `.pin` file (release, build, URL, sha256, the exact `clang --version` line), outside the checkout and verified; `just toolchain-r21b`. The combat script libraries (`src/quest/combat_libs`, `just build-quest-combat-libs`) build on it. See `docs/reference/quest-game-ndk.md`. |
| `quest_stock_data.pin` | The store OBB that `just quest-stock-data` accepts (SHA-256 and size) and the exact tree it must hold, one `file <sha256> <bytes> <path>` line each (ADR 0042). The extractor (`nevr_quest_obb_extract`) and `quest_store_build.py` each read it. |
| `hook_identity_manifest.json` | The pinned prologues `verify_hook_invariants.py` checks. |
| `echomod/` | Standalone Echo VR modding scripts; see `echomod/README.md`. |
