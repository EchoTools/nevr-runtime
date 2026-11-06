# q15t

A Quest plugin for the plugin loader (`docs/adr/0019-quest-plugin-loader.md`): gameplay and fix modules for the pinned libr15
build (build id from `src/quest/sentinel/pinned_targets.h`, `kLibR15StockBuildId`). One library:

- `libq15t.so` (`nevr_q15t`): the modules, loaded in the loader's normal phase.

The heap sizes and the render-target pool are not here: the sentinel sets them in its own early combat step before `android_main` reads
them (`src/quest/combat/heaps.h`, `src/quest/combat/memsys.h`).

The library is in no release or package-dev APK. `just build-quest-q15t` builds it. The unlisted variant APK
(`release-quest-variant-apk`, RELEASING.md) and the combat APK (`release-quest-combat-apk`) package it; each recipe runs `build-quest-q15t` in the
tag's checkout, so the packaged library is that tag's build output, behind that recipe's checks.

## Layout

- `src/` the plugin (`src/<module>.{h,cpp}`, `plugin.cpp` installs them in the order the module docs fix).
- `tests/` host tests: a CMake project, `src/quest/tests/q15t/CMakeLists.txt` (`just test-quest-q15t`, or `just test-quest-q15t <module>`), one `tests/mod/<module>.cmake` per module,
  `src/quest/tests/q15t/mutants.sh` and `src/quest/tests/q15t/mutants.d/` (the mutation run, about an hour, one at a time, not part of the gate).
- `docs/quest/q15t/modules/<module>.md`, `docs/quest/q15t/adr/`: one document per module and the two plugin decisions.
- `CMakeLists.txt`: the options (defaults and the reason for each are in it), added by `src/quest/integration/CMakeLists.txt`.

## Defaults

The build defaults are the shipped behaviour; none of them depends on a config or switch file.

- On: fxtrail, voipdrop, custctx, meshstep (`Q15T_MESHSTEP`, #1555), gunconnect, aibots, bvh, wallfire, impactsnd, music, sensorwd, shaderstep (#1553), instmodel, sendguard, decals (#1619; draws nothing until a `decals.bin` is found), ec, ecfx, droneguard, peerclamp, dronekill (all five words), dronemuzzle, powerboost (`Q15T_POWERBOOST=3`: swallow and cone; #1625), the menu gate
  (`Q15T_GATE_LINK`), the new-profile gun hand (`Q15T_T26`), and the three combat gates
  (`src/quest/plugins/q15t/src/combat_gates.h`): the party-permissions gate (`Q15T_GATE_PERM`, kill file `gate_perm.off`), levelname with boosterpool
  (`Q15T_LEVELNAME`, kill file `levelname.off`) and the bot-body gate (`Q15T_GATE_BOTS`, kill file `gate_bots.off`). The bot-body
  gate is installed only while levelname is: the combat bot body exists only in the full-size global_mp level. One
  `q15t_combat_gates_install` line per run states which of the three are on and why.
- `ec` is on with the words `types=VPTO equip hide` (`Q15T_EC_DEFAULT_WORDS`), and `ecfx` is on (`Q15T_ECFX_DEFAULT_ON`),
  with no switch file. A switch file in the plugin data directory still overrides the words.
- Off: droneguard's second stage (`Q15T_DRONEGUARD_IDX`).
- The menu gate (`Q15T_GATE_LINK`) answers "no kind of headset" only to the 21 script libraries of the combat entries
  (`Q15T_LINK_GATE_LIBS`): the 20 of the combat menu and the lobby script (`libf808c072bb49da47.so`, which loads the lobby's combat
  wing); the other 26 libraries that bind the headset-type expression get the real answer. Each library's first bind logs `headset bind lib=<file> answered=<real|not-a-Quest>`. A `gate_libs.txt` in the plugin data
  directory (first line: `all`, `none`, or comma/space separated library file names) replaces the list.
- The refresh-rate rule (90 Hz, 72 on a Quest 2, on combat maps and in the lobby) and the clock levels are not this plugin's: the runtime
  owns them (`src/quest/combat/refresh_rate.h`, `src/quest/sentinel/clock_levels_hook.h`). The rule's switches are the `combat:` keys
  `refresh_rate`, `refresh_rate_lobby` and `refresh_rate_quest2_72` in `config.yaml`, and the clock raise is
  `diagnostics.clock_levels_override: true`; no kill file or plugin `args` entry switches either.

## Loading

Packaged plugins load with no `config.yaml` entry (ADR 0019 section 2). A `plugins:` entry whose `file` is a packaged library
overrides it (`enabled`, `required`, `args`); an entry cannot clear `early`. A plugin that is not packaged is loaded from the
native library directory only, so a plugin that is not in the APK is not loaded.

## Rules this plugin keeps

- No code page is written: a hook is one GOT word on a data page (`src/quest/plugins/q15t/src/got_swap.cpp` refuses an executable page), because the
  headset's OS denies `execmod` on the game's code pages.
- No `thread_local` in code a hooked game call reaches: the Quest build targets API 26, where the compiler emulates it and the
  first use on a thread allocates. Per-thread state is a `ThreadWord` (`src/quest/plugins/q15t/src/thread_word.h`): a pthread key made at
  the module's install after its other checks, whose value lives in the thread's own TLS block, so a hooked call never allocates. A
  module whose key cannot be made refuses to install and logs `error=<code>`. `src/quest/tests/q15t/check/no_emutls.sh` fails on an
  `__emutls_v.*` symbol (libc++abi's `eh_globals` excepted, by its exact name) in each arm64 module object a `mod/<module>.cmake`
  registers with `q15t_arm64_object` (`just test-quest-q15t`) and in each built library (`just build-quest-q15t`).
- It refuses any libr15 build id other than the one it declares (the NEVR build-id note, and again at `InitEx`), before it
  touches memory.
- No game binary, asset or table is in this tree. The game-derived values are virtual addresses and the single instruction words a
  module pins before it hooks (`src/quest/plugins/q15t/src/word_pins.h` and the per-module pin tables): a pin that does not read back
  refuses the module with one ERROR line.
- Every read of game memory goes through `SafeRead` (`src/quest/plugins/q15t/src/safe_read.h`), so a stale or half-built table is a skipped
  entry, never a fault.
- A wrapper never faults the game: on any doubt it calls the real function.
- A wrapper on a high-fan-in function keeps counters and logs nothing per call.
- Every address, offset and layout is a cited line of the reference build's source or a quoted disassembly word, and anything unread is
  labelled unverified in the module's note (`docs/quest/q15t/modules/`). The audit record `docs/audits/2026-11-05-q15t-origin-citations.md`
  names the source file, line and commit of the citations the plugin's notes and comments carried as `afk_<file>`, `vn`, `pb` and `df` line numbers.
