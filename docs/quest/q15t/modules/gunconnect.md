# Module gunconnect: the local player's guns get `Connect(local user)` so they fire

Port of the Fix in `<source build>/the source build's gunconnect` (header lines 1-24; code `FindCs`, `ReadLocal`,
`HasCombatTag`, `CountGuns`, `FixPass`, lines 185-358). Not ported: the Diag, the tick-callback registry, T-26 (already `weapon_arm.cpp`).
Status: built and host-tested (2026-10-28). Not observed on a headset. Needs a word patch: no.

## Rows
| Row | What | Mechanism here |
|---|---|---|
| R17 | CncaGame::ExclusiveUpdateGame, JUMP_SLOT 0x36e7f28 | none of its own: `GunConnectTick` is registered with `AddGameTick` (game_tick.h); no second hook on the slot |
| R18 | CR15NetGunCS::Connect, JUMP_SLOT 0x36c1ec8 | called, not hooked: `base + 0x2153438` |
| R19 | CR15NetGame::UserActor(u16), JUMP_SLOT 0x36faea0 | called, not hooked: `base + 0x1251654` |
| (none in R1) | CTagCS::HasTag(CncaGameMT*, SActorGUID const&, CSymbol64), JUMP_SLOT 0x36d6170 | called: `base + 0x1723760` |
| (none in R1) | CR15ActorUtils::kActorTagCombat (a CSymbol64), GLOB_DAT 0x3707ca8 | read: `base + 0x37af288` |

## Addresses: source constants against the disassembly (disassembly VA = ELF vaddr + 0x100000)
Source the source build's gunconnect:92: `kFnConnect = 0x2153438, kFnHasTag = 0x1723760, kFnUserActor = 0x1251654, kTagCombat = 0x37af288`;
`:91` `kGunCsVptr = 0x3696050`. They are ELF vaddrs (`base + constant`), the same numbers as in the source build's readelf note (not in this tree):
- Connect: `0000000036c1ec8 ... R_AARCH64_JUMP_SLOT 0000000002153438 _ZN10NRadEngine8NRadGame12CR15NetGunCS7ConnectENS0_13SR15NetUserIdE`.
  Disassembly `functions`: `FN | 2253438 | Connect | 484` (= 0x2153438 + 0x100000). `R1_functions.md` R18: "C1 Connect 0x2153438 -> 0x2253438 (484)".
- UserActor: `...36faea0 ... JUMP_SLOT 0000000001251654 _ZNK10NRadEngine8NRadGame11CR15NetGame9UserActorEt`; disassembly `FN | 1351654 | UserActor | 56`.
- HasTag: `...36d6170 ... JUMP_SLOT 0000000001723760 _ZN10NRadEngine6CTagCS6HasTagEPNS_10CncaGameMTERKNS_10SActorGUIDENS_9CSymbol64E`;
  Disassembly `FN | 1823760 | HasTag | 148`.
- kActorTagCombat: `...3707ca8 ... GLOB_DAT 00000000037af288 _ZN10NRadEngine8NRadGame14CR15ActorUtils15kActorTagCombatE`; disassembly `SYM | 38af288 | kActorTagCombat`.
- Gun CS vptr: `...37057e8 ... GLOB_DAT 0000000003696040 _ZTVN10NRadEngine8NRadGame12CR15NetGunCSE`; vptr word = vtable + 0x10 = 0x3696050 (same convention as
  weapon_arm.cpp's equipment CS, and as `kGameVptr` 0x362ed30 = `_ZTV...CR15GameE` 0x362ed20 + 0x10).
How they are used: `ResolveGunConnectFns` takes `base + vaddr` as the address and asks `dlsym` (libr15.so, RTLD_NOLOAD) for each exported name above; a
name that resolves anywhere but its pinned address is a refusal (ERROR `gunconnect off`, nothing runs), a name that does not resolve is accepted (the start
line says how many of the four were confirmed by name). The source used the fixed addresses only.

## Layout read from the disassembly (instruction words quoted from the live database)
- `Connect` 0x2253438: `ldrh w20,[x0,#0xf2]` (gun count); per gun `ldr x9,[x19,#0xf8]` + `add x20,x20,#0x168` (instances, stride 0x168),
  `ldr x10,[x19,#0xb8]`, `ldr x10,[x10]` (props base), `ldrh w9,[x9,x20]` (u16 at inst+0xa0), `madd x9,x9,x29(=0xa0),x10`, `ldr x10,[x9,#0x20]; cmp x10,x27(=userId & 0xffff)`
  (owner slot), `ldr w9,[x9,#0x9c]; cbz` (flag). It does not read the connect ring or inst+0xd8: those are the source's own conditions (a) and (b), not re-derived here.
- `UserActor` 0x1351654: `and x8,x1,#0xffff; mov w9,#0x250; madd x8,x8,x9,x0; ldr q0,[x8,#0x180]; ...; ldp x0,x1,[sp]; ret`: member stride 0x250, a 16-byte GUID at
  +0x180 returned in x0/x1 (the source's `Guid {u64 lo, hi}` by value), index masked to 16 bits.
- `HasTag` 0x1823760: `mov x19,x2 (tag); mov x20,x1 (guid*); cbz x0 ...; ldr x1,[x20]; ... ldr x2,[x20,#0x8]`: (game, const guid*, tag by value in x2), as the source's `HasTagFn`.
- `CR15NetGame::SendLobbyMemberEvent` 0x135168c builds the same user id and bound the source uses: `ldrh w8,[x0,#0xe2]; cmp w8,w1,UXTH; b.ls` (member count bound),
  `ldrh w8,[x8,#0x178]; and x8,x8,#0x1f` (slot), `ldrh w9,[x9,#0x13a]` (gen at net + slot*4 + 0x13a), `bfm w8,w9,#0x10,#0xf` (id = slot | gen << 16); `ldrh w8,[x20,#0xdc]; cmp ...` (+0xdc is a count).
- `CR15Game::NetGameMT` 0x13027d4: `mov w8,#0x8510; ldr x0,[x0,x8]` (game + 0x8510 = CR15NetGame).
Unverified (source constants only, not re-read in the disassembly): the gun CS key 0x7a804a0a9b2f5386, `+0xde` and `+0x138` of the net game, `inst+0xd8` state values (3 never
processed, 1 disconnected, 0 connected), cs+0x1c0 as the connect ring count. The gamespace table offsets (0x1f48/0x1f78/0x1f88/0x1fb8, gs+0x420/0x450/0x8/0x80) are the shared ones in gamespace.cpp.

## Behaviour (src/quest/plugins/q15t/src/gunconnect.cpp; the source's state machine unchanged)
Every game tick; a pass is taken when `now >= next`; `next = now + 1 s`, shortened to `now + 0.25 s` while any gun CS has unconnected local guns or cannot be read.
A pass finds the gun CSs in both gamespace tables (key, vptr base+0x3696050, cs+0x80 == gs; at most 8), forgets state of CSs that are gone, reads the local member
(`ReadLocal`: net, `+0xde`/`+0xdc` non-zero, slot = u16 net+0x178 & 0x1f, gen, midx not 0xffff and < u16 net+0xe2; user id = slot | gen << 16; guid = `UserActor(net, midx)`),
and for each CS counts the guns (owner slot == local slot, flag u32 props+0x9c non-zero, state inst+0xd8 != 0 is unconnected). Per CS: (a) some local gun unconnected,
(b) ring u32 cs+0x1c0 == 0, (c) `HasTag(game, &guid, *kActorTagCombat)`. All three for 2 s and `now >= next_try`: `Connect(cs, user id)`, `next_try = now + 1 s`, try n of 10.
After the 10th try, the next due pass logs a WARNING and stops for that CS (until it goes away). (a) false after a try: log "connected", tries back to 0. Any of (a)-(c) false:
the 2 s hold restarts. Local member unreadable: every hold restarts. Default ON (the source's default); `<data dir>/gunconnect.off` turns it off.
Online the lobby-member event connects the guns within a frame, so (a) is false and the module does nothing.

Differences from the source (deliberate): off switch is the file only (no setprop); log records are the plugin host's structured lines; extra INFO lines `gunconnect on`,
`gunconnect gun cs` (first sighting of a CS), `gunconnect waiting` (one per distinct reason the local member is unreadable) for the headset run; no pass-cost statistics;
the four addresses are cross-checked by exported name; every read is `SafeRead` (the source's own pipe/process_vm_readv reader).

## Logging (all through `Log`, no per-frame record)
`gunconnect on` / `off` (INFO; ERROR when libr15 is not loaded or a name resolves elsewhere), `gunconnect gun cs`, `gunconnect Connect` (fields: why, cs, tries, unconnected,
local_guns, detail = `user <hex> slot gen midx actor <hi><lo>`), `gunconnect connected`, `gunconnect gave up` (WARNING), `gunconnect waiting`.

## Evidence
`src/quest/tests/q15t/gunconnect_test.cpp` (14 cases): hold, retry spacing, 10 tries and the WARNING, ring, tag (and a zero tag symbol), which guns count (local / flagged / state 1 and 3 / bound),
unreadable local member variants (each reason) and the hold restart, a vanished and returning gun CS, two CSs, tries restart, 1 Hz / 4 Hz rates, key and vptr, `gunconnect.off`,
missing library, address resolution (the four constants and the name cross-check). `src/quest/tests/q15t/mutants.d/gunconnect.sh`: 44 mutants, all killed by a failing check (none by a compile error; checked).
No real-binary run: the plugin can only run inside the game; the headset prediction below is the first real run. NDK r26d `-fsyntax-only -Werror` clean; `llvm-readelf -S`: no `.init_array`.

## Prediction (written before any headset run)
Offline Practice on a build where the local gun does not fire:
1. Log: `gunconnect on` (detail `N of 4 addresses confirmed by exported name`), at level load `gunconnect gun cs ... local_guns>=1 unconnected>=1`, then about 2 s after the combat
   tag appears one `gunconnect Connect ... tries=1 ... detail=user <slot|gen<<16>`, then `gunconnect connected ... tries=1` within about 0.25 s, and the gun fires.
2. Online (or any match where the real event works): `gunconnect gun cs ... unconnected=0` and no `Connect` line.
Falsifiers: (i) `Connect` lines up to tries=10 then `gunconnect gave up` (the call is made but does not connect: wrong argument, wrong ABI for `SR15NetUserId`, or the guns are not
waiting on Connect); (ii) no `gun cs` line while a match runs (key or vptr wrong, or the tick is not running: look for `game tick: object is not the CR15Game`); (iii) `gun cs` but only
`waiting` lines (`why` names the local-member read that fails: `+0xde` / `+0x138` are the unverified ones); (iv) a crash at the first Connect or UserActor call (signature); (v) `Connect`
lines in an online match; (vi) `N of 4 ... = 0` is not a failure by itself (names not exported: fixed addresses used), but a refusal `gunconnect off` ERROR means a name sits elsewhere.
