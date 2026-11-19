# EchoCombat script lifts on Quest: what each reference lift carries

Check: `tools/internal/questlifts` (`go test ./internal/questlifts` in `tools/`). It requires the table under "The lifts" to have exactly 18
rows, each with a distinct 16-hex script hash, a 64-hex sha256 of Cat's image and a 64-hex sha256 of the lift. When `NEVR_QUEST_REFERENCE_LIBS`
names the directory that holds the reference APK's `lib<hash>.so` files (its `lib/arm64-v8a`), it also requires each row's file to hash to the
row's lift sha256. Without the variable that part is skipped. This table is the one list of the 18 scripts.

he_is_the_cat's EchoCombat (branch nevr-pack, 82ad7ba) changes 18 of the game's combat scripts by patching their x64 images, four of them new
scripts his tools make. Main does not ship those images: it rebuilds each change as a hook of its own (AGENTS.md, "Nothing from another
project's patch is taken verbatim"; on PC, for example, `src/runtime/combat/rules_button.h`). A Quest headset runs aarch64 script libraries, and
the reference combat build carries an aarch64 lift of all 18. This page records, for each lift, which of Cat's effects its bytes carry, which
they do not, and which the bytes cannot settle. It is the reference Quest work cites to know which changes a Quest hook
still has to make.

## Inputs

Every input is read as data (`strings`, `sha256sum`, `readelf`, `aarch64-linux-gnu-objdump -d`, `x86_64-w64-mingw32-objdump -d`, byte
searches). Nothing is loaded or run.

- **The reference lifts.** `lib/arm64-v8a/lib<hash>.so` of the reference combat APK: kotorvr's echoquest-combat repository, commit c5a67e8
  ("wave 2"), APK sha256 `be30121cd6d45d62b26058d04b94941ebb143d80d5f92df81517b57a4d4287e1`, 572 `lib<hash16>.so` files. It is the base the pinned
  final-1116 base is composed from (`REFERENCE_APK_SHA256` in `tools/quest_release_base.py`). KNOWN-WRONG #2780: the pinned base replaces
  5 of the 18 lifts here (`6fa8cc9c`, `b7d10bcf`, `d4653fc2`, `e0fdfda9`, `e57332952e1cd478`); the other 13 are byte-identical.
- **Cat's images.** His script DLLs as his 82ad7ba tools write them, one per row, named by the sha256 in the table.
- **Stock images.** The PC game's own script images of the same hashes (`StreamingScriptWin10` resources), where a stock one exists, to tell
  Cat's bytes from the game's.
- **Controls.** The store Quest APK's lifts of the two scripts it has (`lib4b65d1b009973281.so` sha256
  `eebd452fed53650d91a30af18be960e3c3bf4452c22284c32b15839c8ea903b2`, `lib6a0f689e5b91c666.so` sha256
  `d9e0e713f98749ff848e23b4b52614895925872c024ed628d0011e50b3d3ae98`; libr15 build ID `b243509c08ce677aeb95fa348016949b3fc45230`), and the
  reference APK's own lift of the stock detonator script, `lib190ed14fe3da5ed1.so` sha256
  `15a1bf76dd41fa985886855136524a66505937072fe3b767551557c19129dc65`, from which Cat builds the DOT trigger script.

## How a lift reads

A lift is generated C++ compiled for aarch64, with its local symbols kept:

- one `execute_node_<id>` function per script node, `<id>` the node's 32-bit id;
- the transitions in `next_node(node, output)`, or inlined in `execute_thread`: a switch on the node id and the output pin's hash that returns
  the next node;
- the event entry points in `event_handler`, which compares the event's 64-bit hash;
- `re1_xfld`, a table of the game-expression fields the script reads (32 bytes a row: class index at +0x0, name pointer at +0x8, type at +0x10,
  and two words `setup_bindings` hands the engine to fill: the field's offset at +0x14 and its accessor at +0x18);
- nodes the generator kept as x64 code run in an emulator (`xc_m` reads and writes the emulated memory); a call to the engine from such a node
  is a native `blr` with its arguments in place;
- a lib that reads EC's page names `re33_ec_page` (`docs/quest/combat/ec.md`, "What the EC script libraries need"). The RULES mailbox lives on
  that page (`src/runtime/combat/rules_state.h`), so a lib without the name cannot read it.

## Effects

The effect numbers are this page's own (the table below). Cat's tool is named for each; a tool is read as text, never run.

| # | Effect | Cat's tool | Script(s) |
| --- | --- | --- | --- |
| E1 | TEAM READY with more than one player | fix_team_ready.py | 6fa8cc9cd721a563 |
| E2 | PAUSE stops the clocks | fix_clock_pause.py | 6fa8cc9cd721a563 |
| E3 | Starting scores become capture points | fix_capture_points.py | 6fa8cc9cd721a563, b7d10bcffcb2a780 |
| E4 | Capture time T | fix_capture_points.py | b7d10bcffcb2a780 |
| E5 | Unlock time countdown | fix_capture_points.py | b7d10bcffcb2a780 |
| E6 | POINT RATE multiplier | fix_point_rate.py | b7d10bcffcb2a780 |
| E7 | PAUSE gives pre-match orbs | fix_orb_pause.py | e57332952e1cd478 |
| E8 | Station grid cells off canvas | fix_station_cells.py | e0fdfda9a944e8ff |
| E9 | RULES button pressable in Combat | fix_rules_terminal.py | d4653fc2065e8184 |
| E10 | RULES page 2, LOADOUTS | fix_rules_pages.py | d4653fc2065e8184 |
| E11 | Announcer callouts for six gear | fix_vo.py | 7c10e713aec2f12b (+ e0fdfda9a944e8ff events) |
| E12 | TRANSLOCATOR teleport effect and sound | fix_loc_fx.py | 9398a165c004a9c0 |
| E13 | DRONE death explosion | fix_drone_fx.py | 221a0a3a2e56d247 |
| E14 | DOT sound and scene-set errors | fix_dot_script.py | 97d0806e6ea89130 |
| E15 | DOT visible field | fix_dot_field.py | 97d0806e6ea89130 + level data |
| E16 | DOT remote trigger | fix_dot_trigger.py | 4e8af51fe7d6af8c (new) + 97d0806e6ea89130 |
| E17 | Shared magazine skips the frisbee for DOT and DRONE | fix_shared_frisbee.py | 190ed14fe6c74dd3 |
| E18 | DOT dampener, script half | fix_dot_dampener.py | 190ed14fe6c74dd3 + level data |
| E19 | DRONE runs the shared magazine script | fix_drone_script.py | level data |
| E20 | STARBURST realdiv log spam | fix_starburst_div.py | a9db89899292a98f |
| E21 | OVERCHARGE, sixth tac mod | fix_buff.py | 4b65d1b0088d2682 (new), 6a0f689e5b91c666, 4b65d1b009973281 |
| E22 | UMBRAL VEIL, fifth tac mod | fix_smoke.py | 94ba7ca09dcf7b0b (new), ac4e9374a146d3a4, 4b65d1b009973281 |
| E23 | NEUTRON, seventh tac mod | fix_point.py | 94ba7ca39fc97e1a (new), ac4e9374a146d3a4 |
| E24 | Only the equipped tac mod fires | equip_guard.py | 4b65d1b009973281 |

E19 and the level-data halves of E15, E16 and E18 are not in any script lib, so no row below can carry them.

## The lifts

"EC page" says whether the lift names `re33_ec_page`. A count of windows is `sig2`: the 8-byte windows of Cat's image that are in neither the
stock image nor plain byte noise (20 to 44 bits set, at least 7 distinct bytes), and how many of them occur in the lift's bytes or as an
immediate its code builds. Its control runs the same count the other way (stock-only windows in the lift).

| Script | PC name | Cat's image sha256 | Lift sha256 | EC page | Carries | Does not carry | Not settled from the lift |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `190ed14fe6c74dd3` | mp_combat_grenade_core | `374fa592b2c1fc7ba7405c2f15a0a50907bdff72c16a5f63595aeff192a4e57a` | `77b63ed439c9799433eb78c6542557b47475e4dca3a08c8c59cbf67d29b753ab` | no | E17 (below) | | E18 (node 09563fe2 not read) |
| `221a0a3a2e56d247` | mp_combat_ordnance_drone | `9dce02ed6662d4318649694be22239557140eb488837cdf44c60de0eb242eda0` | `93cbcec8e06d32ae4a37fa25dc11ade57692fcb0148b8820615f6f6c166e6ff1` | yes | E13: 1633 of 1653 Cat-only windows; control 0 of 33 | | |
| `4b65d1b0088d2682` | OVERCHARGE (new) | `a40b6a112809a9befd18e6ea4352c60b3a4e4b7de0323c6f1789dd9eac0b4497` | `00d00f28235bffcdf630d9f9af2f6640e727d00f928106b4c2952feb8699245f` | yes | E21: fix_buff.py makes this script and the store Quest APK has no lib of its hash; each of its 86 lifted node ids is a node id of Cat's image | | |
| `4b65d1b009973281` | mp_combat_ability_core | `d32c499a77a9e71a3d737d665593d56140ba6b9a20ea9719f8a468bf06d80e23` | `3d887c46df50e0757ba3cabdb37e403312df28559520ce9f2189f18b54f6ccdc` | yes | E21, E22, E23 chain: the hashes 4b65d1b0088d2682, 94ba7ca39fc97e1a and 94ba7ca09dcf7b0b are 64-bit data at file offsets 0x5b40, 0x5c20 and 0x5c70; the store lift has none of them | | E24 |
| `4e8af51fe7d6af8c` | mp_combat_grenade_dot_trigger (new) | `9505ec6c808fa93dd3745073d08f7594a1496e9dd08806e96195ac427a3c7b2a` | `3825d40a1f9120973f4014df7f387b48f3f30002f03d094716f4a229108a8aad` | no | E16, script half (below) | | E16 level half |
| `6a0f689e5b91c666` | mp_combat_ability_repair_matrix | `65ad9304d7fad2802a244e570e531dfc3d04db75da3468624bd1277aac79036e` | `e6126f81c347e3258e28c16b31e9e5e31991bf7871585ffa2d7799d244a350ba` | yes | E21 heal: 65 of 75 Cat-only windows; control 0 of 28 | | |
| `6fa8cc9cd721a563` | mp_combat_server_state | `14f99b89f4b87a42dfe4287695e822d6f2ff1b6b686e0349a159f4460971c6c6` | `22a3da123a91baaef5e86515756d20b138addaf875bd512a06fc8959ca1bbd52` | no | | E1: `execute_node_7b6a5f5f` reads the flag (`ldrb w21,[sp]` at 0x2b5a8) and sets `cset w24, eq` at 0x2b5b4, the value `!blue_team_ready`, then logs `RE5L_SETFLAG node=7b6a5f5f flag=%d value=%d` (0x802b): the stock toggle Cat makes a constant true. E2, E3 server half: no EC page (inference: no route to the mailbox) | |
| `7c10e713aec2f12b` | mp_combat_voice_hera | `9757a7306cef8f498e2796df86fc0ba132decbd478d36e2056cdbb2c92054cd4` | `f49125c66fd1559bf23ddec0f19578f51ee6f62998a47e75611d574d749281a9` | no | E11: 22 `evt_vo_hra_equip_*` strings against the stock image's 16; the six added are `grenade_loc`, `grenade_dot`, `grenade_drone`, `ability_smoke`, `ability_buff`, `ability_point` | | |
| `9398a165c004a9c0` | mp_combat_grenade_translocator | `c545e908dfdc38957021f33928e12e1b9806de6d6d1c9a9dc2b5114c3e53dd01` | `f05a396765ee5d45c979ac70781aa549c638189d5b88edd806540e0616133fcd` | yes | E12: 1748 of 1766 Cat-only windows; control 0 of 40 | | |
| `94ba7ca09dcf7b0b` | UMBRAL VEIL (new) | `507a5725b8178f93ebe297c6db8821f0a94bceb8d16ed4823ae29d48ecbfa681` | `068130cbfaf5ae3a918d483629a6a1974dd7998632021f20637a2bb37af9fcb6` | yes | E22: fix_smoke.py makes this script and the store Quest APK has no lib of its hash; each of its 29 lifted node ids is a node id of Cat's image | | |
| `94ba7ca39fc97e1a` | NEUTRON (new) | `704a4ee8d2e953bbe92d08fe1f295a08c0d2cf0b56c9ce871e7209c7cf0b8f8c` | `c355711f61cd746b7ea59b3a35fccf47f2e77f0ae82f34fd197d25009861c62f` | yes | E23: fix_point.py makes this script and the store Quest APK has no lib of its hash; each of its 29 lifted node ids is a node id of Cat's image | | |
| `97d0806e6ea89130` | mp_combat_grenade_dot | `c7d9a74fb8bf0f5fe04b6ffacae2b843d975ad04c174cd5fb76cb8aa1ac8e906` | `e3b771dbec526ba9fa638bd29d7f3e64b01d149ca5893c190e70a33e8a641d91` | no | E14, both halves (below) | | E15; E16 DOT side (below) |
| `a9db89899292a98f` | mpl_cbt_wpn_integrated_custom_rifle | `845a39db877126fcb668aef69eef0deac231f5421465c4c61bf871139b572bfe` | `cbe347deaa63b733b27b995b7e35a74512c27c95b72437d5bf682b2987cab407` | no | E20 (below) | | |
| `ac4e9374a146d3a4` | mp_combat_ability_barrier_actor | `e05626dd88cc61278460292de8354efc0818dc93a2197376d79b9c2f2069a4e3` | `ba87a9d0b354669002f46f28bf47398613d413f5fbfd49a1ae5f9149bbc0309d` | yes | E22, E23 wall half: 2887 of 2934 Cat-only windows; control 0 of 49 | | |
| `b7d10bcffcb2a780` | mp_combat_gamemode_capturepoint | `0f79fbc6ca85fdafc9b8d88b03bb9520137fc1a9a90b592ff15a8e7c6797cf6b` | `92ccfa6111e7ae8894d6ec2fca4f5af67ae81392c854423922a5905035bc8870` | no | | E3, E4, E5, E6: no EC page (inference: no route to the mailbox) | |
| `d4653fc2065e8184` | mpl_arena_match_podium_manager | `9392f46b19a23c47918cf5af3a632ed97bf58ed12b38cae036596eb1bb31ce1d` | `0b1119269bd8d8d719a92a02cd229510a80a1a827cbd65fda211ec1f36626946` | no | | E9 (below); E10: no EC page (inference: no route to the mailbox) | |
| `e0fdfda9a944e8ff` | mp_combat_equip_podium_logic | `369eb8b3fb18ff3ab9dd7989b989f7c19ced8475c6109faa0e71550d07f49634` | `debe6caed8f1992c445abcfa90cba5c4c966cb8e06e3586f1102f92842883ac7` | no | | E8: no EC page (inference: no route to the mailbox) | E11 events |
| `e57332952e1cd478` | tst_dsn_josh_cbt_utility_player_spawn_point | `e692bda9b06c56256d71393fe97d9d3d25c35f3d7f30446a5c5b4c8539dc0d2b` | `9576e91cfebd10568d34318eaf5271e6a234d1dcb2d248caa36f083f2c5f36d5` | no | | E7: no EC page (inference: no route to the mailbox) | |

## Five rows read in full

### E9, the RULES button: not carried

Cat's fix_rules_terminal.py replaces each call of the engine's flag accessor after a read of the `matchisarena` or `matchiscombat` field id
with `mov byte [r8], 1` (arena) or `0` (combat), so the podium terminal reads "Arena, not Combat". PC does the same with two slot replacements
(`src/runtime/combat/rules_button.h`).

In the d4653fc2065e8184 lift, `re1_xfld` row 63 names `matchisarena` (name relocation at 0x3e558, string at 0x8d18) and row 64 `matchiscombat` (0x3e578,
string at 0x77bc). `setup_bindings` hands the engine each row's +0x14 and +0x18 words to fill (0x105f8 to 0x10608). Every read of the two
flags loads the accessor the engine stored and calls it, with nothing written over its output afterwards:

- `matchisarena` (accessor word 0x3e568, offset word 0x3e564): `execute_node_50a6de4e` (`ldr x8,[x8,#1384]` at 0x2740c, `blr x8` at
  0x27428) and `execute_node_874ed767` (0x2bf60, 0x2bf7c);
- `matchiscombat` (accessor word 0x3e588, offset word 0x3e584): `execute_node_37181b2c` (0x24e8c, `blr` at 0x24ea4, its output byte read by
  `ldrb w8,[sp,#208]` at 0x24eac), `execute_node_50a6de4e` (0x27444), `execute_node_874ed767` (0x2bf98), `execute_node_bcfa9321` (0x303ec,
  0x30404) and `execute_node_deb69873` (0x334f4, 0x3350c).

So the lift reads the real flags, as the stock terminal does: 2 arena reads and 5 combat reads, where the PC image has 4 and 10. The two
accessor words are data words of the lift's `.data`, which is where a Quest hook would act. The time rows are stock too: Cat blanks the
`:00` and `00:` pieces, and `execute_node_9d4ae12a` builds the `00:` prefix inline (`mov w10,#0x3030` at 0x2dac8, `mov w10,#0x3a` at
0x2dad4). The canvas half of the tool (the MINUTES and SECONDS labels) is level data, not in the lift.

### E14, the DOT scene-set half: carried

Cat's fix_dot_script.py rewires node 393f4ada's next node from 9507dc91 to 013166c6, skipping the three EnableModelSceneSet nodes
(9507dc91, cd089ec5, 878f54cb). In Cat's image the stock `mov eax, 0x9507dc91` at image offset 0x10aaf is `mov eax, 0x013166c6`; the stock image's other
`mov eax, 0x9507dc91` (0x1120d) is still there, and Cat's image has one more, at 0x11f50.

In the 97d0806e6ea89130 lift, `next_node(0x393f4ada, output)` returns 0x013166c6 for the default output 0x33f5911416f09172 and for output 0 (0xfe28 to
0xfe48), and −1 for any other. The one transition into 9507dc91 left is from node f2a71917 (0xf954 to 0xf974), which matches the `mov eax,
0x9507dc91` Cat's image keeps. The three scene-set nodes are still compiled (`execute_node_9507dc91` 0x1c730, `execute_node_cd089ec5` 0x22524,
`execute_node_878f54cb` 0x1c314), as they are still in Cat's image.

The sound half is carried as well: the lift holds Cat's event ids as data (`0x5b47a1cd` at file offset 0x583f, `0xbde3209d` at 0x52df) and logs
`RE7_PSND 97d0806e f2a71917 sound 5b47a1cd` and `RE7_PSND 97d0806e 5fefe254 sound bde3209d`; neither stock id (`0xf7c92b80`, `0xc9cd3996`)
occurs in the lift.

### E16, the DOT remote trigger: script half carried

Cat builds `mp_combat_grenade_dot_trigger` (4e8af51fe7d6af8c) from the detonator script 190ed14fe3da5ed1 with eight byte changes (three event hashes, five transitions), and attaches it
to the DOT magazine in five levels' `CScriptCRWin10`. The trigger lift was compared with the reference build's own lift of the stock detonator,
by evaluating each lift's transition switch (`execute_thread`) for every node id it compares and every output hash it compares, and each
`event_handler`'s event compares:

- **Event handler.** The detonator lift compares `delegate_onspawnedremote` 0x0360ce7dfc51dd43, `delegate_ondeposited` 0x088725a83299f252 and
  `delegate_ontypechanged` 0x71b428190a37733e. The trigger lift compares none of them, and compares 0x9a7dadde0d7dd001, 0x227dadde0d7dd001 and
  0xaa7dadde0d7dd001 in their place: Cat's never-matching values (`01 d0 7d 0d de ad 7d` and the low byte of the patch's RVA).
- **Transitions.** Of the detonator lift's transitions, exactly these differ in the trigger lift, and each is one of Cat's rewires:
  - 75014b69 → 70415af5 (onspawnedlocal, after the trigger is enabled): absent, the thread ends;
  - f8ab569b → 7fd4b29c (autorun): absent;
  - 8935bc5b → 6521d5a6 (server onstatechanged): absent;
  - b0c42820 on the activated state e32dc7dfd49f50a9 → ac4df5ec, and on Exploded 0x33f5841b26fb8162 → cabf6a85: both → 5a04a5e5 (disable the
    trigger for the owner);
  - b61a3584 on output 0x71b428190a37733e → cabf6a85: → 5a04a5e5 (the same patched block of Cat's x64 is reached from this case too).

  Every other transition of the 49 nodes is the same in both lifts.
- **The DOT side.** The 97d0806e6ea89130 lift holds the export 2fc48a8b48754599 (`script_function_exists`, `script_function_call`, nodes 23bf8eb2 and
  6702f075), the four external function hashes (a150b1c25e86ac18, 048bcc7d19348703, 048bcc7d19398c03, 4cc1130866b48fc1) and the activated state
  e32dc7dfd49f50a9. These are the DOT script's own plumbing; Cat changes none of it.
- The trigger lift also carries the reference build's own log lines (`RE6_TRG ...`, `RE6_TRG v1 nodes=8 self=g+0xb0`, `RE6_TRG cap n=%08llx`);
  the detonator lift carries the `RE6_DET_ONESHOT` lines both have.

Not decidable from the lifts: the attachment of 4e8af51 to the DOT actor in the levels' script table, and the `trigger-nostick` settings
(`use_remote_trigger` on, `activate_on_collision` and `stick_to_geo` off) the tool asks for. Both are level and balance data, which the Quest
combat data does not carry. Whether the field then holds still is the dampener's (E18), which Cat marks unverified.

### E17, the shared magazine's frisbee: carried

Cat's fix_shared_frisbee.py turns node 93def80d's condition "type != arc" into "type is none of arc, dot, drone", on the low 16 bits of the
type symbol (arc 0xf0e9, dot 0xedfe, drone 0x4ab5).

In the 190ed14fe6c74dd3 lift, `execute_node_93def80d` reads the type with `ldrh w8,[sp,#96]` at 0x38b64 and compares it with 0xf0e9, 0xedfe and 0x4ab5
(0x38b68 to 0x38b84), so a full 64-bit CSymbol64("dot") is never built (the 16-bit form is Cat's). Any match returns output
0x54dc9983c968aa39, none returns 0x56ccd1bd99a59d3b (0x38bdc to 0x38c7c). `next_node(0x93def80d, ...)` sends 0x56ccd1bd99a59d3b to e8657155,
the node that enables `CR15FrisbeeCR`, and 0x54dc9983c968aa39 to 0x193def80d, not to e8657155. So the frisbee is enabled only for a type that is
none of the three.

### E20, STARBURST realdiv: carried

Cat's fix_starburst_div.py NOPs the 13 log calls that write `realdiv(%x) divide by zero`, leaving the division and its arguments as they were.

In an unpatched lift the call is a native one: in the 97d0806e6ea89130 lift, `execute_node_9ebf5994` puts the format's address in the emulated register at
0x1d998 and, when the emulated target is the engine's log, calls it with `blr x8` at 0x1d9c8 (`w1 = 8`, `x2` the native copy of the format at
0x5dd2). That lift holds the format twice (0x4d30, 0x5dd2).

The a9db89899292a98f lift holds the format once (0x130a0). Nine code sites load its address (`execute_node_be578d0b` 0xbd7d4, `execute_node_d9a3e735`
0xd5874, `xc_2ec90` 0x104f84, three in `re1_hc_4bfa3c0f`, three in `re1_hc_5d8e3857`; the tenth reference, in `xc_inrdyi`, is the emulator's
table of readable ranges). At each site the channel 8 is stored as well, and the path that follows, read for 80 instructions through
unconditional branches, makes no call but the emulator's memory reads (`xc_m`) and `memcpy`. In `execute_node_d9a3e735` it goes on to the
clamp at 0xd58f4. So the log call is gone and the result is unchanged, as in Cat's image.

## What these bytes do not settle

- E18 (node 09563fe2 in 190ed14fe6c74dd3), E24 (the selection on the EC page in 4b65d1b009973281), and the e0fdfda9a944e8ff half of E11.
- Whether an effect the server's script carries (E1, E2, E3, E6) reaches a Quest client from a PC dedicated server: a live run decides it,
  not the lifts.
- Everything Cat changes in level data: E10's canvas, E15's model data, E16's script table entry and settings, E18's dampener data, E19.
- "Does not carry" for a lift without the EC page is an inference from the page being a lift's only route to the mailbox; no lift was searched
  for another route.
