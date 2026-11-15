---
# atwd read contract
read: whole
read_via: cat docs/adr/0047-arena-is-the-default-build-and-combat-is-opted-into.md
audience: all
---

# ADR 0047: arena is the default Quest build, and combat is chosen by asking for the combat build

Status: accepted. Applies to the Quest release recipes and the Quest sentinel's constructor sequence. The PC runtime installs combat as it always did, with no opt-in. Amends ADR 0046 rule 5 (combat-area code that shipped to every APK) and the kinds of #2298 (which Quest APK is the
default build). Issue #2661.

## Context

The project is built for competitive arena, and combat must never cost arena stability. A tester froze and crashed on combat
builds while staying out of combat, and had none on a build without it. Two things put combat code in front of every Quest player:

- **The default Quest recipe.** `just release-quest-apk <tag>` repacked the pinned reference combat build (#1994,
  `tools/quest_release_base.py` `REFERENCE_APK_SHA256`): its `libvrapi.so` runtime, its script libraries and its switch file
  ride along, and the repack re-pins that runtime to our libr15 (`tools/internal/questruntimepin`). The arena APK (#2298) existed,
  but only under its own recipe name.
- **Three Quest hooks on every launch.** The combat name tags (`src/quest/combat/match_tags.h`), the equipment re-fire
  (`src/quest/combat/equip_refire.h`) and the grey irradiance default (`src/quest/sentinel/irradiance_default_hook.h`) came from
  the combat build and installed whatever the APK's variant, outside `combat::Active()` (ADR 0046 rule 5).

## Decision

1. **The default Quest build is arena.** `just release-quest-apk <tag>`, with no other argument, repacks the store APK
   (`$NEVR_STORE_APK`) with the release's loader asset and stamps it `arena`: the same body and output as
   `release-quest-arena-apk` (`build/quest-release/<tag>/arena/`). `tools/quest_release_base.py check-arena` refuses the APK before
   it is written if it holds any entry of the reference build, the combat plugin's library, a plugin manifest, or a `libvrapi.so` that carries
   the reference runtime's libr15 checksum table (`questruntimepin.CarriesTable`; the store's own `libvrapi.so` has none).
2. **A combat Quest build exists only when it is asked for by name.** `release-quest-combat-apk` and `release-quest-variant-apk`
   (the store APK, stamped with a combat variant word, ADR 0040) and `release-quest-reference-apk` (the reference combat build, #1994). The release
   timer's `build_apk` step names `release-quest-reference-apk`, so the APK the manifest lists as `quest-apk` is the one it was;
   which APK an installer gets is not changed here.
3. **The three every-launch Quest hooks are combat-only.** The constructor sequence (`src/quest/integration/ctor_sequence.h`) runs
   `install_match_tags` and `install_equip_refire` (their own steps, right after `install_permission_wait`) and
   `install_irradiance_default` only when `combat::Active()`; otherwise each is skipped with the reason `combat_inactive`, as
   `install_combat_early` is. Skipped, the irradiance step installs neither its GOT hook nor its row's handler, and the row stays
   pass-through (ADR 0021: a hook that is not wanted stays pass-through).
4. **PC combat is not opted into.** The PC runtime installs combat (`nevr_combat::Install`, `nevr_combat::RegisterBalance`) as it always
   did, with no config key; the opt-in in this ADR is the Quest build choice only.
5. **Defaults live in code.** The Quest default is the variant word the recipe stamps. Nothing ships a `config.yaml` line to switch on
   what ships.

## Finding a violation

- **D1.** A recipe that repacks the reference build, or stamps a combat variant, without the reference or combat name in its own.
- **D2.** A Quest hook from the combat build that installs in a run where `combat::Active()` is false.
- **D3.** A shipped `config.yaml` that sets `combat.force`.

## Consequences

- ADR 0046 rule 5's exception for `match_tags` and `equip_refire` is gone: every combat-area module is behind `combat::Active()`.
- The manifest lists the reference repack as `quest-apk` and the arena APK as `quest-apk-arena`; making arena the kind installers
  give by default is #2662.

## Alternatives

- **Keep the reference build as the default and gate inside it.** Rejected: its `libvrapi.so` runtime is binary code we do not
  build, and it acts in every level.
- **Gate the three Quest hooks inside `InstallPermissionWaitHook`.** Rejected: a step of their own is reported by the sequence
  (`skipped`, `combat_inactive`) and driven by its host test with fakes.
- **A PC opt-in (`combat.enabled`).** Rejected: PC has always had combat, and the arena default is a Quest build choice.

## Not claimed

- That the PC equipment re-fire (`src/runtime/patch/equipment_refire.h`) and the netgun bullet transforms (#1420) are arena-neutral;
  they are the runtime's own, not the combat module, and this ADR does not gate them (#2663). The same holds for the PC lobby-settings
  hook (`nevr_lobby_settings_hook::Install`, `src/runtime/lifecycle/initialize.cpp`), which came with the combat port and installs on
  every client; it passes the game's call through unchanged.
- A headset or game run of any of this; the host tests and the recipe tests drive it.
