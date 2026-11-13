# Module `headset_gates`: the party-permissions gate, the bot-body gate and the menu gate

Files: `src/quest/combat/headset_gates.{h,cpp}`, `src/quest/combat/headset_gates_targets.h` (the pins), `src/quest/combat/headset_gates_entry.inc` (the two
arm64 entries), `src/quest/combat/combat_gates.h` (which gates a run installs), `src/quest/combat/check/headset_gates.sh` (the arm64 checks),
`src/quest/tests/headset_gates_test.cpp`, `src/quest/tests/combat_gates_test.cpp`, `src/quest/tests/headset_gates_library_name_test.cpp`,
`src/quest/tests/headset_gates_qemu.cpp`, `src/quest/tests/got_pinned_test.cpp`. The module is a step of the sentinel's normal combat step, registered last
(`src/quest/combat/combat_install.cpp`): it runs in the combat APKs and in no other. Switches (`config.yaml`, `combat:`): `gate_perm`,
`gate_bots`, `gate_link` (default true each) and `gate_link_libs` (default the 21 libraries below). No key names levelname: the bot gate asks the game (below).

Origin: the combat plugin's `headset_gates.cpp` and `combat_gates.h` (imported in #1591), reconstructed on the hook manager. `docs/audits/2026-11-05-q15t-origin-citations.md`
lists no reference-build file or EchoCombat line for the gates, so none is cited.

## What the game does, and what each gate answers

The game asks "is this a Quest" with two imported functions, `CVR::HeadsetType()` (JUMP_SLOT 0x36f9fd8, real function 0x1a6f164, which is `mov w0,#3; ret`:
this build always answers 3) and `CR15NetGame::PartyMemberHeadsetType(uint)` (JUMP_SLOT 0x36fd880, real function 0x129b168). Three places in libr15 branch on the
answer. A gate reads the return address of the call, which is data, and answers a different value to that one caller; every other caller gets the real answer.
No code page is written. All addresses are ELF virtual addresses; the disassembly's are 0x100000 higher.

| Gate | The caller (a range; the return address is inside it) | What the game does with 3 | The gate answers |
|---|---|---|---|
| `gate_perm` | `CR15NetPartyPermissionsExpression::operator()` [0x232c794, 0x232c854); `bl PartyMemberHeadsetType` at 0x232c810 returns to 0x232c814 | `cmp w0,#3; b.eq` (0x232c818) clears the combat permission flag (+1 of the output) of that party member | 0 (not a Quest) |
| `gate_bots` | `CR15NetGame::BotActorId(CSymbol64)` [0x124dd38, 0x124e004); `bl CVR::HeadsetType` at 0x124dd40 returns to 0x124dd44 | `cmp w0,#3; b.ne` sends a Quest to bot body 0, the arena bot, before it looks at the bot's symbol | 0 (not a Quest) |
| `gate_link` | `BindExpression<CR15HeadsetTypeExpression>` [0x202ba74, 0x202bac8); `bl CVR::HeadsetType` at 0x202baa0 returns to 0x202baa4 | the kind (0..4) becomes the four flags the menu scripts read (`cmp w0,#4; b.hi`, then a jump table); 3 sets the Quest flag, which locks the combat cards | 4 (the PC build's Link kind: sets the link flag and not the Quest flag) |

The permissions expression only ever clears flags. Its second flag (+1) is cleared when the member's kind is 3, or when it is not 3 and `PurchasedCombat(PartyMemberId)` is
false; so with the gate a Quest member who bought combat keeps the flag and one who did not loses it, the same as any other headset. The first flag (+0) does not read the
headset at all.

The Bind opens `sub sp,sp,#0x20; stp x19,x30,[sp,#0x10]` and calls from that frame, so the library of the script that holds the Bind is the one that contains the word at
(the Bind's entry sp) + 0x18. The entry hands its continuation x0 = the caller's sp and x1 = the caller's return address; `dladdr` names the library; the policy
(`gate_link_libs`: `all`, `none`, or name fragments separated by commas or spaces) decides. A library that cannot be named is not in a list. The default policy is the 21
libraries of the combat entries: the 20 of the combat menu (the combat buttons, FIND COMBAT, the combat mode card, the level select pages, the "combat allowed" prefab)
and the lobby script `libf808c072bb49da47.so`, whose two LoadLevel branch nodes read the expression's Quest output to choose the lobby wing that is loaded (a Quest gets
the arena wing only; the Link answer takes the combat wing's pin). The other 26 libraries that bind the expression (47 in all) get the real answer. The first bind of each library and its 64th are
logged (`combat_gates_headset_bind`).

## The two assembly entries

A `CallbackThunk` handler sees its own thunk's return address, not the game's, so each slot holds a reviewed assembly entry (`src/quest/sentinel/callback_thunk.h`
rule 7): a top-level `asm()` block with CFI (never `naked`) that has no pass-through path. It takes what the continuation needs (a jump touches neither x30 nor sp) and
tail-branches with x30 and sp as the caller left them, so the continuation returns straight to the caller. The continuation calls the function the manager published in
the entry's next word with an acquire load (`ldar`), then decides. Both entries are installed through `InstallAsmEntry` from records declared with
`NEVR_ASM_ENTRY_RECORD`; the frame sensor in `tests/quest` reads both and lists them in `reviewedAsmEntries`. `check/headset_gates.sh` compiles the entries for arm64,
compares them with the expected instructions and runs them under qemu-aarch64.

`CVR::HeadsetType`'s one entry serves `gate_bots` and `gate_link`; a gate that is off, or refused, has an empty range. `gate_perm` has the other slot.

## Another handler on the slot (#2219)

A gate acts on the game's own call site, so its entry stays the newest hook on its slot. The manager installs the entry above any handler already on the slot (the entry
calls it, and it calls the game's function), and refuses a later hook that asks to join above the entry (`slot_held_by_entry`, a Warning that names both owners). In both
orders the gate keeps answering (`headset_gates_test`).

## Arena

Arena and combat run on the same APK, so `combat::Active()` does not say which one a player is in.

- `gate_bots` acts only where levelname is installed (the game's LoadLevelName vtable word, `headset_gates_targets.h` `levelNameSlot`, is not the stock function: the full-size global_mp, which carries body 1, loads) and only while a combat map or the combat lobby is loaded (`src/quest/combat/combat_level.h`). For a bot symbol `BotActorId` does not list, the game picks the
  bot body at random, and body 1 (the combat bot) does not exist in an arena level, so the gate answering "not a Quest" in an arena match would hand arena bots a body
  that is not there. In an Echo Arena level, in every other lobby level and before the frame tick has seen the game, the gate answers the real value, and the first eight such calls are
  logged with their reason (`combat_gates_scoped_out`: `levelname_not_installed`, `echo_arena`, `other_level`, `no_game`). The level table cannot say whether the cut level is loaded: `CncaGameSpace::LoadLevel` stores the requested name (`global_mp`) at `gs+0x58` before the virtual LoadLevelName appends `_lowspec`, so the slot is the signal. This is where the module differs from the plugin, which answered every call.
- `gate_perm` acts wherever the expression runs. It changes one flag of a party member's combat permission, for a Quest member who bought combat; it does not touch arena
  play, and a member who did not buy combat gets the game's own answer.
- `gate_link` acts for the policy's libraries wherever they run. The lobby script is shared by the arena and combat lobbies, so an arena player in the combat APK has the
  combat wing loaded in the lobby too. That is how a combat entry is reached from the lobby (the library is on the default list for that reason); whether it costs the arena
  lobby time or memory is not measured (no headset run).

## Lines

See the header of `headset_gates.h`: `combat_gates_hook`, `combat_gates_link_policy`, `combat_gates_install`, `combat_gates_answered`, `combat_gates_scoped_out`,
`combat_gates_headset_bind`, `combat_gates_legacy_switch`. The plugin's `gate_perm.off`, `gate_bots.off` and `gate_libs.txt` are no longer read; one found in its data directory
is a Warning that names the key that replaces it.

## Pins

`src/quest/combat/headset_gates_targets.h` holds both slots, both real functions, the three ranges and 13 instruction words (each gate refuses alone when one differs).
`src/quest/tests/got_pinned_test.cpp` resolves both relocations and compares every word in the store libr15 (`just test-quest-hooks-pinned`, which needs the store APK).
