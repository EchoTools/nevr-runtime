# q15t

A Quest plugin for the plugin loader (`docs/adr/0019-quest-plugin-loader.md`): gameplay and fix modules for the pinned libr15
build (build id from `src/quest/sentinel/pinned_targets.h`, `kLibR15StockBuildId`). Two libraries:

- `libq15t.so` (`nevr_q15t`): the modules and the clock hook, loaded in the loader's normal phase.
- `libq15t_early.so` (`nevr_q15t_early`, option `Q15T_EARLY`): `heaps` and `memsys`, which must act before `android_main`
  reads the heap sizes, so they are their own library and, when packaged, an `early` plugin. Whether the early phase runs on a
  headset is ADR 0019 P10 and P14; the loader refuses the entry loudly (`early_unsupported`) when it does not.

Neither library is in a release or package-dev APK. `just build-quest-q15t` builds them; the unlisted variant APK
(`release-quest-variant-apk`, RELEASING.md) packages them.

## Layout

- `src/` the plugin (`src/<module>.{h,cpp}`, `plugin.cpp` installs them in the order the module docs fix).
- `tests/` host tests: `src/quest/tests/q15t/run_host_tests.sh` (`just test-quest-q15t`), one `tests/mod/<module>.sh` per module,
  `src/quest/tests/q15t/mutants.sh` and `src/quest/tests/q15t/mutants.d/` (the mutation run, about an hour, one at a time, not part of the gate).
- `docs/quest/q15t/modules/<module>.md`, `docs/quest/q15t/adr/`: one document per module and the two plugin decisions.
- `CMakeLists.txt`: the options (defaults and the reason for each are in it), added by `src/quest/integration/CMakeLists.txt`.

## Defaults

The build defaults are the shipped behaviour; none of them depends on a config or switch file.

- On: fxtrail, voipdrop, custctx, meshstep (`Q15T_MESHSTEP`, #1555), gunconnect, aibots, bvh, wallfire, impactsnd, music, sensorwd, shaderstep (#1553), instmodel, sendguard, ec, ecfx, the per-script menu gate
  (`Q15T_GATE_SCRIPTS`, `scriptgate`), `Q15T_SHIELDGAMMA=2`, and the new-profile gun hand (`Q15T_T26`).
- `ec` is on with the words `types=VPTO equip hide` (`Q15T_EC_DEFAULT_WORDS`), and `ecfx` is on (`Q15T_ECFX_DEFAULT_ON`),
  with no switch file. A switch file in the plugin data directory still overrides the words.
- Off: droneguard, peerclamp, dronekill, dronemuzzle, hudmeter, levelname (with its companion boosterpool), `Q15T_POWERBOOST=0`, `Q15T_GATE_PERM`, `Q15T_GATE_BOTS`, `Q15T_GATE_LINK` (the all-scripts menu gate that `scriptgate` replaces).
- The clock hook logs the levels the game asks for and forwards them unchanged. `args: {clock: raise}` in a `plugins:` entry of
  `config.yaml` raises the CPU and GPU levels to at least 4 (`src/quest/plugins/q15t/src/plugin_args.cpp` reads the flat JSON of string values).

## Loading

Packaged plugins load with no `config.yaml` entry (ADR 0019 section 2). A `plugins:` entry whose `file` is a packaged library
overrides it (`enabled`, `required`, `args`); an entry cannot clear `early`. A plugin that is not packaged is loaded from the
native library directory only, so a plugin that is not in the APK is not loaded.

## Rules this plugin keeps

- No code page is written: a hook is one GOT word on a data page (`src/quest/plugins/q15t/src/got_swap.cpp` refuses an executable page), because the
  headset's OS denies `execmod` on the game's code pages.
- It refuses any libr15 build id other than the one it declares (the NEVR build-id note, and again at `InitEx`), before it
  touches memory.
- No game binary, asset or table is in this tree. The game-derived values are virtual addresses and the single instruction words a
  module pins before it hooks (`src/quest/plugins/q15t/src/word_pins.h` and the per-module pin tables): a pin that does not read back
  refuses the module with one ERROR line.
