# POWER BOOST, the client half (rows R51..R56)

Files: `src/quest/combat/powerboost.{h,cpp}`, `src/runtime/patch/powerboost_rules.h` (the marker decode, the cone and the link markers,
shared with PC's `src/runtime/combat/powerboost.cpp` and tested on PC by `src/runtime/tests/test_powerboost_rules.cpp`),
`src/quest/combat/powerboost_core.h` (the link line's filter), `src/quest/combat/powerboost_targets.h`,
`src/quest/combat/powerboost_thunk.inc`, `src/quest/combat/thread_word.h`, `src/quest/combat/check/powerboost.sh` and
`powerboost_thunk.want`, `src/quest/combat/mutants.d/powerboost.sh`, `src/quest/tests/powerboost_test.cpp`, `src/quest/tests/pb_thunk_qemu.cpp`.

An EchoCombat server puts POWER BOOST's messages into the heal channel: a heal amount of 1,000,000 or more is never a heal but a marker
("I used it", "you are linked", "unlinked"). Without this module those markers reach `CR15NetDamageableCS::Heal` as enormous heals.

## Switch and default

`combat.powerboost_stage` in `config.yaml`, an integer 0..3, default **3** (swallow and cone): what kotorvr's echoquest-combat ships (its wave-2
recipe, `qa-1104-aj4-w2z22.json` in its trials directory, lists an empty `powerboost.on`, whose words are `swallow cone`, `afk_powerboost.cpp:89`). A value
that is not an integer in 0..3 keeps 3 with a Warning that names the key. `0` installs nothing. No file in the app's data directory switches
the module.

| stage | what is installed | what it does |
| --- | --- | --- |
| 1 | HealNode::Enter, Heal (JUMP_SLOT), SetTargetInsideBubble | records where markers pass and what the link call is given; every call goes on untouched |
| 2 | and Heal's GLOB_DAT word | a marker never reaches Heal; a "used it" marker is sent as `DamageFromClient(cs, handle, -amount, 0, 0)` |
| 3 | and ResolveBody's JUMP_SLOT (an assembly entry) | a target outside the owner's +z cone gets a far stand-in body; link / unlink markers go out from the EC client's net tick |

Stage 3 needs the EC client's net tick (`ec::OnNetTick`). When the registration refuses, stage 3 is refused with an Error
(`combat_powerboost_cone_off`) and stage 2 stays. Stage 2 needs `DamageFromClient`, `ResolveBody` and `PrimaryBody` at their pins and the sender's
slot holding `DamageFromClient`; when not, stage 1 stays, with an Error.

## Rows

ELF vaddrs of the store libr15 (`readelf -rW`; the disassembly's address is the ELF address plus 0x100000). `got_pinned_test` checks each.

| row | function | word | callers (llvm-objdump of the store libr15) |
| --- | --- | --- | --- |
| R51 | `CR15NetDamageableHealNode::Enter` | JUMP_SLOT 0x36e3868 -> 0x23207b0 | one: `BindNode<..HealNode>` (0x23c2c6c) |
| R52 | `CR15NetDamageableCS::Heal` | JUMP_SLOT 0x36f7820 and GLOB_DAT 0x3725b80 -> 0x2137e74 | the JUMP_SLOT two: `ClientDamageDealtCB` (0x2137a44) and HealNode::Enter (0x23208b0); the GLOB_DAT word one reader, HealNode::Enter's queued heal (`ldr x8,[x8,#0xb80]` at 0x2320904) |
| R53 | `CR15NetHealBubbleCS::SetTargetInsideBubble` | JUMP_SLOT 0x36d7d08 -> 0x2158954 | one: `UpdateActiveHealBubble` (0x2157b98) |
| R54 | `CR15NetDamageableCS::DamageFromClient` | JUMP_SLOT 0x36e5778 -> 0x213e80c | called, not hooked; the slot is read |
| R55 | `CPhTarget::ResolveBody` | JUMP_SLOT 0x36ca098 -> 0x19a27ac | 771 `bl` to its PLT stub 0xee5360 |
| R56 | UpdateActiveHealBubble's per-target `bl 0xee5360` | code word 0x97b63672 at 0x2157998 | not patched: the entry recognises its return address, base + 0x215799c |

`CPhysicsCS::PrimaryBody` (JUMP_SLOT 0x36f6110 -> 0x1677d34) is called by the cone, not hooked.

Every hook goes through the hook manager: four CallbackThunks installed with `InstallThunk`, and ResolveBody's assembly entry installed with
`InstallAsmEntry`. Each slot must hold the function at its pin; a slot another hook took first is refused. Stage 1's three words go in together
or not at all: a later refusal removes the earlier hooks through the manager.

## The assembly entry

`powerboost_thunk.inc`: a top-level `asm()` block with CFI (callback_thunk.h rule 7), 17 instructions. It reads the call-site word
`nevr_combat_pb_cone_ret` and the next word `nevr_combat_pb_next` (the manager's) with `adrp` / `add :lo12:` / `ldar` at every call. A call
whose return address is not the cone's goes on with `br x16`, every argument register, x30 and sp as the caller left them (10 instructions:
`bti`, two `adrp`/`add`/`ldar`, `cmp`, `b.eq`, `br`). The cone's call gets a frame and runs the continuation
`nevr_combat_pb_cone_at(x0 = the CPhTarget*, x1 = UpdateActiveHealBubble's sp, x2 = its x24)`, which calls the next word for the body (an
acquire load) and returns the body or the stand-in. The entry stays the newest hook on its slot: a later handler is refused (`slot_held_by_entry`).

`check/powerboost.sh` (run by `just test-quest-combat powerboost`) compiles the entry with the NDK, requires exactly the listing in
`powerboost_thunk.want`, follows each `cmp x30, <r>` and `br <r>` of the entry and the continuation's first `blr` back to the instruction that set
the register and requires `ldar` or `ldapr`, checks that the sentinel's object carries the same text, and runs `pb_thunk_qemu` under
`qemu-aarch64`: an ordinary call reaches the next function with x0, x1, x2, x30 and the callee-saved registers intact; the armed call reaches the
continuation with the target, the caller's sp and x24; a return address one instruction off is an ordinary call.

## The rules

`powerboost_rules.h` holds the rules both ports run, from EchoCombat v2.0.0's `plugin/powerboost.cpp`. A marker's value is what is past its
band's base, rounded (`+ 0.5f`), so 1,020,009.75 decodes to dr 20, 10 s. A use or link amount whose rounding carries it out of its band (1,999,999.75 gives
v 1,000,000) is malformed and is declined: no buff, no link, never sent to the game's Heal once markers are swallowed (stage 2; the read-only stage 1 swallows none), and a use already running is left alone (EchoCombat's decode would end that
use, and its link marker for the linked teammates is misread as a use that ends their one second of immunity). The script's heal node writes whole amounts, which decode the same with or without the rounding. The angle in a bubble's
radius names POWER BOOST's bubble at 1..180 degrees and MEDIGUN's at 501..590; the Quest has no MEDIGUN and leaves that bubble, and any other, to the
game's own radius check, as it does a radius that is not finite.

## Arena

The q15t and combat APKs also play Echo Arena, and the module is installed in both. It acts only while a combat map or the combat lobby is
loaded: the game tick asks `combat_level::Classify` once every 32 frames (and when the CR15Game changes) and keeps the answer in an atomic;
before a tick has seen a level the answer is "not combat". Outside that scope every wrapper only calls the game's function, the entry's
continuation returns ResolveBody's own body, and nothing is logged, held, swallowed, queued or sent. When `combat_scope` (#2246) lands,
`InScope()` reads it instead.

## Lines

The `combat_powerboost_*` events in `powerboost.h`. The install line names the stage that is on, the stage asked for and the consequence; a
refused hook is an Error with its GotStatus; the per-kind caps (markers 96, links 160, swallows 96, sends 160, cone 240 at most twice a second,
scope changes 64) are announced once by a `combat_powerboost_capped` line.

## Headset row (owed)

On a q15t or combat APK, in a combat match on a server that runs EchoCombat's PowerBoost plugin, with someone using POWER BOOST:
`combat_powerboost_install` with `"stage":3`; `combat_powerboost_scope` with `"in_combat":1`; `combat_powerboost_use` (`"why":"queued node heal"`)
for a use of ours; `combat_powerboost_swallow` for the server's relayed markers; `combat_powerboost_cone` lines with `LINK` / `no`; and
`combat_powerboost_link_marker` with `"sent":1` after a use of ours. In an arena match on the same APK: `combat_powerboost_scope` with
`"in_combat":0` and none of the acting lines.

## What the tests do not show

The entry inside the game; the byte layout of the game's body and bubble record (+0x8f8 position, +0xbd0 quaternion, record +0 / +8 / +0x38,
frame +0xe8 / +0xa0), which are the reference build's and not decoded independently; DamageFromClient's behaviour on a server; how often
ResolveBody runs per frame on a headset.
