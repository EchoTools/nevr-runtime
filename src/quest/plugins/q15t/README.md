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
- `docs/quest/q15t/modules/<module>.md`: one document per module.
- `CMakeLists.txt`: the options (defaults and the reason for each are in it), added by `src/quest/integration/CMakeLists.txt`.

## Defaults

The build defaults are the shipped behaviour; none of them depends on a config or switch file.

- On: levelname with boosterpool (`Q15T_LEVELNAME`, kill file `levelname.off`).
- Gun connect, the practice AI bots and the gun hand default are not this plugin's: the runtime owns them (`src/quest/combat/gunconnect.h`, `aibots.h`,
  `weapon_arm.h`). Their switches are the `combat:` keys `gunconnect`, `aibots`, `aibots_scenario` and `weapon_arm` in `config.yaml`, each on by default; no kill file
  or data file switches any of them. The marker files of the gun hand stay in this plugin's data directory, `files/q15t` (player state).
- The impact sounds and the level decals are not this plugin's: the runtime owns them (`src/quest/combat/impactsnd.h`, `src/quest/combat/decals.h`). Their
  switches are the `combat:` keys `impactsnd`, `impactsnd_write`, `decals`, `decals_flip_u`, `decals_flip_v` in `config.yaml`; the plugin's `decals.off`,
  `decalflipu.on` and `decalflipv.on` are no longer read.
- The static BVH and the wall-fire check are not this plugin's: the runtime owns them (`src/quest/combat/bvh.h`, `src/quest/combat/wallfire.h`). Their
  switches are the `combat:` keys `bvh`, `bvh_gate`, `bvh_touch` and `wallfire` in `config.yaml` (all on by default); no kill file switches either.
- The three headset gates (party permissions, bot body, menu link) are not this plugin's: the runtime owns them
  (`src/quest/combat/headset_gates.h`). Their switches are the `combat:` keys `gate_perm`, `gate_bots`, `gate_link`, `gate_link_libs` in
  `config.yaml`; no kill file or data file switches any of the three gates. The bot-body gate acts only where this plugin's levelname is installed
  (it reads the game's LoadLevelName slot).
- `ec` and `ecfx` (the EC network client and NEUTRON effects) are the runtime's (`src/quest/combat/ec.h`), with the `combat:` keys `ec`,
  `ec_words` and `ecfx` in `config.yaml`; the plugin's `ec.on`, `ec.off`, `ec_preset*.on` and `neutronfx.on` files are no longer read.
- POWER BOOST is not this plugin's: the runtime owns its five words (`src/quest/combat/powerboost.h`). Its switch is the `combat:` key
  `powerboost_stage` (0 to 3, default 3: swallow and cone) in `config.yaml`.
- Combat music and the scanner-sphere watchdog are not this plugin's either: the runtime owns them (`src/quest/combat/music.h`, `src/quest/combat/sensorwd.h`), switched by the
  `combat:` keys `music` and `sensorwd` (both on by default) in `config.yaml`; no kill file switches either.
- The wrist HUD's heat fill and the shield colour are not this plugin's: the runtime owns them (`src/quest/combat/hudmeter.h`, `src/quest/combat/shieldgamma.h`),
  switched by the `combat:` keys `hudmeter` (on), `shieldgamma_tube_mirror` (on) and `shieldgamma_gamma` (off) in `config.yaml`; no `hudmeter.on` file or build option switches either.
- The crash and timing guards (fxtrail, voipdrop, custctx, droneguard, peerclamp) are not the plugin's: they are modules of the runtime's combat area (`src/quest/combat/`, `docs/quest/combat/`), switched by `combat.<module>` keys in `config.yaml` (docs/adr/0046).
- dronekill and dronemuzzle are not this plugin's: the runtime owns them (`src/quest/combat/dronekill.h`, `src/quest/combat/dronemuzzle.h`).
  Their switches are the `combat:` keys `dronekill` and `dronemuzzle` in `config.yaml`.
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
