# The drone explosion

A combat drone that dies explodes on this client: one shot down (the EC server's 'K') at once, one that runs out (its own end) when its
damageable goes off. On PC the first system its script turns off is held back until 2.5 s after the explosion; on the Quest that hold is a
switch of its own, `combat.dronekill_hold`, off by default (below, "The hold"). The Quest's half is `src/quest/combat/dronekill.cpp` (the
switches `combat.dronekill_explosion` and `combat.dronekill_hold`); PC's is `src/runtime/combat/dronefix.cpp`; the queue, its timing and the expiry rule are one definition both
compile, `src/runtime/patch/dronekill_rules.h` (`FxQueue`, `kFxSlots`, `kFxSoundHoldMs`, `OnDamageableOff`, `ExpiryLetGo`, `kNcaPhysics`).

The explosion reads four words of the EC page's drone block (`re33_ec_page`, page+0xd60): a counter, the context of the last drone node, and
the spawn and sound entries the drone library's prelude stores there. This document records what the built library stores, what the name of
the drone's sound key is, how the Quest module uses them, and what is not established.

## Origin

The reference build is `kotorvr/echoquest-combat` at `c5a67e8`. The facts come from these files of that build:

- `runtime/native/afkxr/afk_dronekill_core.h`: the drone actor, the damageable and the sound key as symbol hashes (`kDroneActor`, `kDamageable`,
  `kDroneSound`, labelled DRONE_SOUND), and the spawn and sound offsets (`kDroneCtx`, `kDroneSpawn`, `kDroneSoundFn`).
- `runtime/native/afkxr/afk_dronekill.cpp`: the explosion's use of the page, and the damageable's ON Enter.
- `libsrc/221a0a3a2e56d247/221a0a3a2e56d247.cpp`: the lifted prelude of the drone library (the stores at the drone block, the two entries).

Our copies: `src/quest/combat/ec_core.h` (the offsets, next to `kDroneBlock`, and the entries' library and offsets) and
`src/quest/combat/check/drone_explosion_page.sh` (the check of the built library). The PC port reads the same words in
`src/runtime/combat/dronefix.cpp` (`DEATH_TOKEN_RVA`, `DroneCtx`, `SpawnDeathFx`) and its keys in `src/runtime/patch/dronekill_rules.h`
(`kDamageable`, `kNcaPhysics`).

## What was established

Measured in the library that `just build-quest-combat-libs` builds from that libsrc (`lib221a0a3a2e56d247.so`, NDK r21b, disassembled with the
NDK's `llvm-objdump`). The prelude computes the drone block from the page pointer in `x0` (`add x9, x0, #0xd60`) and stores:

| Field | Page offset | Store (address in the built library) | Value |
|---|---|---|---|
| counter (node steps) | +0x18 | `add x8, x0, #0xd78` (8b84), then `ldaxr`/`stlxr` on `[x8]` (8bb4) | incremented by one |
| context of the last node | +0x20 | `add x1, x9, #0x20` (8bd0), `str x19, [x0]` (8c0c) | `x19` loaded from the stack (`ldr x19, [sp, #0x28]`) |
| spawn entry | +0x28 | `add x1, x9, #0x28` (8c28), `str x19, [x0]` (8c60) | `0xb8d0`, the symbol `re33_ecent_221a0a_13720` |
| sound entry | +0x30 | `add x1, x9, #0x30` (8c7c), `str x19, [x0]` (8cb8) | `0xbb14`, the symbol `re33_ecent_221a0a_13920` |

The stores go through the library's window write (a `bl` between the address and the store). The check `src/quest/combat/check/drone_explosion_page.sh`
asserts this sequence against the offsets in `src/quest/combat/ec_core.h`.

The two entries are local symbols of the library (`llvm-nm`: `t`) and are not in its dynamic symbol table (`llvm-nm -D --defined-only` lists
neither), so `dladdr` cannot give their names. The Quest module therefore checks an entry word by its library and offset: `dladdr`'s
`dli_fname` ends in `kDroneLib` (`lib221a0a3a2e56d247.so`) and the word minus `dli_fbase` is `kDroneSpawnEntry` (0xb8d0) or
`kDroneSoundEntry` (0xbb14). The check script asserts both offsets against the symbols and that neither name is dynamic.

The drone's sound key, `0x5b8cc538e22ad937`, is `CSymbol64("ncaPhysics")`. The transliteration of the hash in `sdk/include/nevr/symbol_hash.h`
reproduces the four vectors in that header. The repository's symbol table names the same value `ncaPhysics`
(`src/runtime/log/symcache_data.cpp`). The upstream and EchoCombat's `DRONE_SOUND` call this key the drone's sound key; the shared header
names it `kNcaPhysics`, and PC's `dronefix.cpp` uses that constant.

The damageable key, `0xc9dc08cfc2f5920f`, is `CSymbol64("R14NetDamageable")`. In `echovr.exe`, the PC port's enable-enter hook is the function at
`ENABLE_ENTER_RVA` in `dronefix.cpp`, and its prologue bytes match the function at that address in the game (checked in ReVault).

PC constants for the same words, by symbol name in `src/runtime/combat/dronefix.cpp`: `DEATH_TOKEN_RVA` (0x2104D60, the same base as the
prelude's drone block), `+ 0x20` in `DroneCtx`, slot 0 at `+ 0x28` in `SpawnDeathFx`, and slot 1 at `+ 0x30` in the loop of `SpawnDeathFx`. The
counter at +0x18 has no PC constant.

## Not established

- Which script is the drone script. The key `0x5b8cc538e22ad937` occurs as an immediate in eight script DLLs of the game and as a component key in
  `echovr.exe`; none of them was identified as the drone's script.
- The symbol the sound entry uses. The lifted source of `re33_ecent_221a0a_13920` loads `0xc8c33e48297612b6`. That value is not
  `CSymbol64("POINT")`, which is `0xe32dc7cad6864aa4`; the upstream calls the entry "POINT's sound" without a symbol.
- The PC kDamageable branch: that the PC enable-enter hook (`Hooked_EnableEnter` in `dronefix.cpp`) takes the damageable branch for
  `dk::kDamageable` the way kotorvr/echoquest-combat's afk_dronekill.cpp does.
- That the context at +0x20 is the drone node's context at run time. The lifted source (`rbx = rcx` at entry) and the stack slot give it;
  no run of the library was made.
- Whether the key `0x5b8cc538e22ad937` is used in the drone's graph as a sound system or as the physics system its name gives. The name is
  `ncaPhysics`; the role in the drone's graph is open.
- What `CEnableComponentNode::Enter` (ELF 0x273b700, VA 0x283b700) does for a disable besides turning the resolved component off (vtable
  +0x210, at once or through the game space's deferred queue when the owner's +0x368 is 0). That read is ReVault's; the function is not in the proven
  reconstruction tree, so that is not established. The hold skips that whole Enter for one node (PC runs
  the real Enter and defers only the disable), so the hold is safe only if that disable is Enter's one effect. Until the reconstruction shows
  it, `combat.dronekill_hold` is off by default.
- That the context the page holds at a drone's release (kept with its group) is still that drone's context when it explodes, with two drones
  out. The hold line's `ctx_match` says, per hold, whether the page's context at the first-system off equals the one kept at the release.
- What becomes of a shot-down drone's explosion entries on a server (Quest is a client: no server branch).

## The Quest module

### Switches and defaults

- `combat.dronekill_explosion`, a boolean, default true (the reference build ships `dronefx.on`): a dead drone's explosion, on a 'K' and at
  its end. It acts only with `combat.dronekill` on: the explosion rides on dronekill's Enter hook, its group table, its kill feed and its tick
  (`dronekill::Words::explosion`). False: no explosion is queued, and the install line says `explosion=0`.
- `combat.dronekill_hold`, a boolean, default **false**: the first-system off is held back and done 2.5 s after the explosion
  (`dronekill::Words::hold`). It acts only with the explosion on. It stays off until the reconstruction of `CEnableComponentNode::Enter`
  shows that the disable is that Enter's one effect ("Not established"); turning it on by default is one constant, the default of
  `CombatConfig::dronekillHold` in `src/quest/sentinel/quest_config.h`. The install line says `hold=0|1`.

### What it hooks

Nothing new. dronekill's `CEnableComponentNode::Enter` handler (in a combat map only) does these more things:

| Event | Action |
|---|---|
| the damageable's ON (a release) | keep the page's `kDroneCtx` word with the drone's group (0 = none) |
| the first-system off (`kNcaPhysics`, enable 0), the hold off | the real Enter runs, untouched; then its actor names the drone's group (the one live group of that actor whose end is not seen), whose expiry opens with nothing held (`end_open`; none or two such groups: `end_not_open`) |
| the same, the hold on, exactly one group can still run its end (one whose damageable off is not seen: alive, or dead, however long ago it was released: nothing bounds when a shot-down drone's end script runs, so a dead group stays a candidate until its own end is seen), it is alive, its component resolves under its actor and game space, the system's DisableComponent is the read shape and its game space's invoke-immediate word reads | the real Enter is not run; the component is held |
| the same, the hold on, any of that not so (no such group, two or more, the one dead, the component not resolved, the shape or the word not readable) | the real Enter runs at once (the game's behaviour); a `not_held` line; then the expiry opens by actor as with the hold off |
| the damageable's off, the group was alive, its expiry open, a context kept | the explosion is queued; with a held component, that goes off 2500 ms after the explosion (`sound_delay_ms` 2500), else nothing is held (`sound_delay_ms` 0) |
| the damageable's off, the group already dead (shot down) | a held component goes off at the next tick, no explosion |
| the damageable's off, no first-system off seen for the group | nothing: the game's own end, no explosion (as on PC, where the expiry opens at that off) |
| a hold no damageable off closed within 2500 ms | the held component goes off |
| a release that takes over the slot of a group with an open hold, or a drone node in another game space that clears the table | that held component goes off at the next tick (at once, from the handler, when the off list is full) |
| a 'K' for a live group (kill word on) | the explosion is queued, holding nothing; an open hold goes off at the next tick |
| a second 'K' for the same drone | nothing (the shared kill rule finds no live group) |
| the queue full (8) | a held component goes off at the next tick; Warning `queue_full` |
| the scope ends (3 asks in a row, 32 frames apart, read no combat map) with holds open, queued or waiting to go off | dronekill forgets them and nothing turns them off, so they stay on; one Warning `combat_dronekill_hold` line, step `dropped`, with the `count`. One or two such answers end nothing (a read that failed while the map is loaded is not the map ending); each is a Debug `combat_dronekill_scope_wait` line. That their game space is gone is not established |

The explosion runs at dronekill's tick (the game thread, outside any script node): both entry words are read from the page and each must be
the drone library's entry (above), the kept context must be readable, its state word at +0x18 is saved, zeroed before each entry and put back
after both. A held component goes off through dronekill's stage-1 path (`OffOne`), retried while its game space defers the disable (owner
+0x368 = 0), at most 2 s (`kDueMs`).

**A held component can stay on.** When `OffOne` cannot turn it off (the component looked up again is not the one held, the game space defers
the disable for 2 s, or the off list is full and the game space defers), the component stays on and nothing turns it off: the game's own
Enter for it did not run. One Warning `combat_dronekill_hold` line, step `stays_on`, names the cause in `why`. A hold is taken only when the
checks that make `OffOne` succeed pass at the time of the hold, so this is the case where the game's state changed between the hold and its
off.

### Log lines

- `combat_dronekill_explosion` (Info): `index`, `group`, `reason` (`killed|expired`), `sound_delay_ms` (2500 or 0), `entries` (2), `msg`; the first
  64 of a run, then one `combat_dronekill_log_cap` line; the stats record's `explosions` keeps counting.
- `combat_dronekill_explosion_skipped` (Warning): `index`, `group`, `reason` (`queue_full|no_entry|foreign_entry|no_context`), `entry` (the word
  refused, or 0), `msg`; the first 16.
- `combat_dronekill_hold`: `index`, `group`, `step`, `why`, `ctx_match` (1, 0, or -1 when not measured), `msg`. Steps `held`, `released` (it
  went off), `off_next_tick` (handed to the tick's off list), `let_go`, `not_held` (`why`: `no_live_group`, `ambiguous`, `dead_group`, `already_held`, `no_system`, `no_shape`, `no_disable_shape`,
  `owner_unreadable`, `no_component`, `group_changed`), `end_open` and `end_not_open` are Debug, the first 64; `stays_on` is Warning, the
  first 16 (its own budget); `dropped` is Warning, with `count` and `no_level_asks`, once per scope that ends with held components.
- `combat_dronekill_stats` gains `explosions` and `held`; `combat_dronekill_install` gains `explosion` and `hold`.

### Arena cost

None added. The Enter handler's out-of-scope path is unchanged (one relaxed load and a tail call); the explosion's checks run only inside
`EnterInScope`, which runs only in a combat map, and the tick's work only while something is due.

### Tests

`src/quest/tests/dronekill_test.cpp` (`TestExplosion`, `TestExplosionHoldOff`, `TestExplosionDeadEndsWhileAnotherLive`,
`TestExplosionHoldNeedsOff`) drives the real handler through the real thunk, the fake entries and a fake `dladdr`
answer; `src/runtime/tests/test_dronekill_rules.cpp` (`Fx*`, `Expiry*`) pins the shared queue and expiry rule with the same vectors;
`src/quest/combat/mutants.d/dronekill_g.sh` and `dronekill_h.sh` hold the module's mutants.

### Headset row

Quest 3, the combat APK, against an EchoCombat server, with the default (hold off): shoot a drone down (its explosion and sound at once,
`reason=killed`); let one run out (its explosion, `reason=expired sound_delay_ms=0`, after an `end_open` line). Two drones out at once: each
explodes at its own position. With `combat.dronekill_hold: true` (only after the reconstruction of `CEnableComponentNode::Enter`): let one
run out alone (its explosion, then whatever the held component controls goes off 2.5 s later: record whether that is the drone's sound or
its physics, and the hold line's `ctx_match`).

### Platform

PC: present, `src/runtime/combat/dronefix.cpp` (`QueueDeathFx`, `DeathFxDue`, `ExpiredDamageable`), which runs the same `FxQueue` and expiry
rule from `src/runtime/patch/dronekill_rules.h`. PC runs the real Enter of the first-system off and holds only its disable, by its own disable
hook, per thread, and picks the drone by the node's context. The Quest has no disable hook: with the hold off it runs the real Enter and
opens the expiry by the Enter's actor (the explosion without the delay); with the hold on it skips the Enter and picks the drone by the only
group that can still end. PC lets a hold go only at that thread's next first-system off; the Quest also lets it go 2500 ms after the hold
(`ExpiryLetGo`, which only the Quest calls).
