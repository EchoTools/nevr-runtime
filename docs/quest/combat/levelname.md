# Modules `levelname` and `boosterpool` (rows R04 and G7, the reference build's "G7")

Files: `src/quest/combat/levelname.{h,cpp}`, `levelname_targets.h` (the pins), `boosterpool.{h,cpp}`, `boosterpool_targets.h`,
`src/quest/tests/levelname_test.cpp`, `boosterpool_test.cpp`, `src/quest/tests/got_pinned_test.cpp` (the pins in the real libr15) and the mutant lists
`src/quest/combat/mutants.d/levelname.sh` and `boosterpool.sh`. Both are normal combat steps (`src/quest/combat/combat_install.h`): they run in the `q15t`
and `combat` APKs and in no other, after the tick and the refresh-rate rule, levelname first. One switch covers the pair, `combat.levelname` in
`config.yaml` (docs/adr/0046 rule 4, default true): the booster pool's substitution is right only while the full-size level is the loaded one, so it
installs only when levelname did (`needs_levelname` otherwise). The plugin's kill file `levelname.off` is not read; one found is a Warning
(`combat_levelname_legacy_switch`) naming the key. The module needs a full-size global_mp level in the pack.

Origins: the rule is the reference build's G7 (`qcp_patches.py:386` of its tools, pinned in `docs/audits/2026-11-05-q15t-origin-citations.md`, section 5, the row for
`levelname.cpp:13`); it rewrote the `tbnz` branch target into a code cave. The companion is the reference build's word row
`booster_pool.plain_name` (`tbz` to `b` at 0x2110f40, stock `0x36f8012a`, replacement `0x14000009`; the record pins no line for it). Both are reached
here through the dynamic relocations instead of a code write, in our own code.

## Stock function (the disassembly, `instructions`)

```
1302ecc 085b8f52 mov   w8, #0x7ad8
1302ed0 086868b8 ldr   w8, [x0, x8, LSL #0x0]
1302ed4 4800f837 tbnz  w8, #0x1f, 0x01302edc
1302ed8 3e41f417 b     0x010133d0
1302edc e8cc00f0 adrp  x8, 0x2ca1000
1302ee0 08d92f91 add   x8, x8, #0xbf6              ("_lowspec")
1302ee4 22008052 mov   w2, #0x1
1302ee8 23008052 mov   w3, #0x1
1302eec e00301aa mov   x0, x1
1302ef0 e10308aa mov   x1, x8
1302ef4 ebbbf417 b     0x01031ea0
```
`functions`: `1302ecc|LoadLevelName|44`. Decompilation (raw): `void CR15Game::LoadLevelName(long param_1, undefined8 param_2)`:
`if (-1 < *(int *)(param_1 + 0x7ad8)) { CncaGame::LoadLevelName(); return; } CSymbol64::Append(param_2,"_lowspec",1,1);`.
So this = x0, name (CSymbol64, 64 bit, in x1); nothing else is read. The `_lowspec` string at disassembly 0x2ca1bf6 is
referenced from exactly two functions (`xrefs`): `LoadLevelName` (1302ef0) and `CR15NetBoosterCS::AllocateExclusive` (2210f54).

Callees, read the same way:
- `10133d0` (the plain tail call): `functions` says `thunk_FUN_018f162c | NRadEngine::CncaGame | 16`; bytes are a PLT stub:
  `adrp x16,0x37e1000; ldr x17,[x16,#0xd0]; add x16,x16,#0xd0; br x17`. Its GOT word is ELF 0x36e10d0, `readelf -rW`:
  `00000000036e10d0 R_AARCH64_JUMP_SLOT 00000000017f162c _ZNK10NRadEngine8CncaGame13LoadLevelNameENS_9CSymbol64E`.
  The target `18f162c` (size 8) is `mov x0, x1 ; ret` (`e00301aa c0035fd6`): returns the name, `undefined8 f(param_1,param_2)`.
  So the plain function is (this, name) -> name, both in registers, nothing on the stack; a PLT stub is safe to call as a
  function pointer (x16/x17 are the scratch registers it uses).
- `1031ea0` (Append) is likewise a PLT stub, JUMP_SLOT ELF 0x36f0638 -> `_ZN10NRadEngine9CSymbol646AppendES0_PKcjj`.
  Not called by the levelname handler (the stock function does that); the booster pool's handler calls it.

Register contract of the handler: `uint64_t Handler(Fn original, void* game /*x0*/, uint64_t name /*x1*/)`, result in x0. Nothing in
x2.. is read by either callee (the stock function sets w2/w3 itself for Append), so none is forwarded.

## The six data xrefs (disassembly `xrefs`, to_address 1302ecc), and what they are

ELF vaddr = disassembly VA - 0x100000; `readelf -rW` and `readelf -SW` of the pinned library, the store APK's `lib/arm64-v8a/libr15.so`
(`readelf -n`: `Build ID: b243509c08ce677aeb95fa348016949b3fc45230`, the pinned id).

| Disassembly VA | ELF vaddr | section | what |
|---|---|---|---|
| 3350f34 | 3250f34 | .eh_frame_hdr (0x3249210..) | NOT a vtable word: 4 bytes `bc9cfbfd` = 0xfdfb9cbc = 0x1202ecc - 0x3249210 (the unwind search table entry) |
| 3428e10 | 3328e10 | .eh_frame (0x330ed50..) | NOT a vtable word: an FDE `pc_begin`, pc-relative 0xfdeda0bc: 0x3328e10 + (-34758468) = 0x1202ecc |
| 372ef90 | 362ef90 | .data.rel.ro | `R_AARCH64_ABS64 0000000001202ecc _ZNK10NRadEngine8NRadGame8CR15Game13LoadLevelNameENS_9CSymbol64E + 0`; in `_ZTVN10NRadEngine8NRadGame8CR15GameE` (0x362ed20, size 0x420) at +0x270 |
| 3730e58 | 3630e58 | .data.rel.ro | same reloc; `_ZTV...CR15NetDedicatedServerE` (0x3630be8, size 0x420) +0x270 |
| 3731278 | 3631278 | .data.rel.ro | same reloc; `_ZTV...CR15NetLanServerE` (0x3631008, size 0x420) +0x270 |
| 37316f8 | 36316f8 | .data.rel.ro | same reloc; `_ZTV...CR15NetServerE` (0x3631488, size 0x420) +0x270 |

The four relocations are `R_AARCH64_ABS64` against the exported symbol (value 0x1202ecc, addend 0), not `R_AARCH64_RELATIVE`. At run time the word is
therefore the symbol's resolved address, which is base + 0x1202ecc unless another loaded object defines the same name first (then the word differs and the
install refuses). `.data.rel.ro` is inside GNU_RELRO (0x35f9eb0 + 0x132150), so the pages are read-only at run time and the manager's write makes them
writable and back (exercised by the host test, "read-only vtable page").

Vtable slot: the symbol + 0x270; the vptr (address point) is symbol + 0x10, so the virtual offset is 0x260 (index 76).

## Which class dispatches, which vtables are swapped

Class tree (typeinfo base pointers, from `.data.rel.ro` relocations, checked with a script over all 3285 exported `_ZTV` symbols):
`CncaGame -> CRxGame -> CR15Game -> CR15NetServer -> {CR15NetLanServer, CR15NetDedicatedServer}`. Every class whose RTTI chain reaches
CR15Game has an exported vtable and each of these four holds CR15Game::LoadLevelName at +0x270; no class of the tree
overrides it. The two classes above CR15Game (CncaGame 0x3659ee0, CRxGame 0x36a2a88) hold `CncaGame::LoadLevelName`
(`17f162c`) in that slot and are not touched. Which of the four the running game object is on a Quest client is
**unverified**; all four are hooked, so it does not matter.

The caller (the only dispatch through slot 0x260 in a CncaGame/CR15Game/CncaGameSpace/CR15Net* function; the other `#0x260`
loads in those namespaces are `CR15NetGame::Broadcast<...>`, a different object): `CncaGameSpace::LoadLevel`
(`18e5484`):
```
18e5528 ldr x0, [x19, #0x8]      ; x19 = the gamespace, +8 = its CncaGame (the game)
18e552c mov x1, x22              ; x22 = the requested level name (CSymbol64)
18e5530 ldr x8, [x0]             ; vptr
18e5534 ldr x8, [x8, #0x260]     ; slot
18e5538 blr x8
18e553c ldrb w8, [x21, #0x1] ; 18e5540 mov x22, x0   ; the answer is the name the archive loader gets
```
The decompilation shows `uVar6 = (**(code **)(**(long **)(param_1 + 8) + 0x260))(*(long **)(param_1 + 8),param_2);` and uVar6 goes
to `CArchiveLoader::LoadArchiveAsync` / `CResourceInstance::LoadAndInit`. The function has no `bl` xref and no other
dispatch was found by the instruction scan (`ldr xN,[xM,#0x260]` followed within 3 instructions by `blr xN`: 3 non-PLT hits,
LoadLevel, `OutputTestsUI` with x1 = xzr and an unrelated generic wrapper `FUN_0297efe0` taking w0 back; neither is
this call). An indirect call from a place that scan cannot see is unverified.

## Mechanism and behaviour

`levelname::InstallAt` finds libr15, publishes the plain function (base + the PLT stub 0xf133d0, ELF) and installs one thunk
(`sentinel::InstallThunk`, ADR 0046 rule 2) into each of the four ABS64 words (ELF 0x362ef90, 0x3630e58, 0x3631278, 0x36316f8), each target pinned
to its word, each word required to hold `base + 0x1202ecc` (`expectedOriginal`: a word that holds anything else is refused, not chained over). Four words
hold one function and the thunk keeps its original in a static, so each word has its own thunk and its own `GotHook` handle (four registry entries,
`hook_budget.h`). All four or none: a refusal on any word takes the ones already installed out again (`GotHook::Remove`, a store that only succeeds
over its own thunk), disarms every thunk and writes one Error line that names the word, its status and how many were put back. A second install is
refused (`already_ran`); a refused one leaves nothing installed and may be tried again.

Handler: `name == 0x3F9915D3001DC28E` and bit 31 of the 32-bit word at game+0x7ad8 (read with `combat::SafeRead`, `process_vm_readv`, so an
unreadable object reads as clear) -> call the plain function with the same two arguments and answer what it answers; anything else -> the word's own
original with the same arguments. With the bit clear the stock function returns the plain name itself, so the handler only changes the answer in the one
case the stock appends "_lowspec". The line for a substitution is written after the plain function returned, never while game code is on the stack
above the handler (`callback_thunk.h` rule 1).

`boosterpool::InstallAt` installs one thunk into the JUMP_SLOT of `CncaGameSpace::AllocatePooledActor` (ELF 0x36cd1f8, the slot of the PLT stub at
0xeeb620; the word must hold `base + 0x18037c4`). The handler recognises the one name the stock builds, `CSymbol64::Append(plain, "_lowspec", 1, 1)` (the
PLT stub at 0xf31ea0, called as the stock does; `plain` is the u64 at 0x37af868, which AllocateExclusive loads with `adrp x11, 0x37af000; ldr x20,
[x11, #0x868]`), and asks the function for the plain name instead; every other name from every other caller reaches the function unchanged. The two names are
read and computed at the first call that can read the plain name and kept (a plain name of 0 is the game's static not set yet: used for that call, not
kept), so a later call costs one atomic load and one compare. An unreadable plain name passes the call through and is counted.

## Lines (sentinel log)

```
combat_levelname_install  {"result":"installed|refused|off","reason":"ok|off_config|<GotStatus token>|module_not_loaded","key":"combat.levelname","slots":4,
                           "failed_slot":"none|cr15_game|dedicated_server|lan_server|net_server","cr15_game":"ok",..,"rolled_back":0,"rollback_failed":0,"consequence":".."}
combat_levelname_kept     {"name":"0x3f9915d3001dc28e","n":1,"lowspec":1,"answer":"plain name"}   Info, the first 16; KeptPlainCount() keeps counting
combat_levelname_legacy_switch  Warning, `levelname.off` found
combat_boosterpool_install {"result":"installed|refused|off","reason":"ok|off_config|needs_levelname|<GotStatus token>|module_not_loaded","key":"combat.levelname","slot":"<GotStatus>","consequence":".."}
combat_boosterpool_kept    {"name":"0x..","n":1,"answer":"plain name"}   Info, the first 16; KeptCount() keeps counting
```
Info when installed or off, Error when refused. A real record from the host test, a refused word:
```
"event":"combat_levelname_install","result":"refused","reason":"original_mismatch","key":"combat.levelname","slots":0,"failed_slot":"net_server","cr15_game":"ok","dedicated_server":"ok","lan_server":"ok","net_server":"original_mismatch","rolled_back":3,"rollback_failed":0,"consequence":"global_mp loads as the game's cut level (global_mp_lowspec) when the lowspec flag is set; the combat bot body is not in it"}
```

## What an arena match gets (the cost of the pair on the same APK)

Arena and combat run on the same APK, so every combat module is installed while a player is in an arena match.

- **levelname cannot be scoped by level.** global_mp is the game's global level: it loads once after login (`docs/reference/game-level.md`: "loading
  global", the level field is none and only the menu and global levels are loaded), before any lobby, so nothing says yet whether the player goes to an
  arena or a combat match and the loaded-level answer (`combat_level.h`) is "none". On a q15t or combat APK with `combat.levelname` on (the default), an
  arena match runs with the full-size global level too: the same global level and its combat content (the combat bot body and the booster pool
  actor) resident, where a plain release loads the cut one. The handler does nothing for any other level, and nothing for a game whose lowspec bit is clear
  (that game already loads the full level). What the full level costs a Quest in arena (memory, load time) was not measured; the headset row below measures
  it. `combat.levelname: false` leaves the game's cut level (and the booster pool with it). The bot-body gate (`headset_gates.md`) follows the key
  without reading it: it acts only where the game's LoadLevelName vtable word is not the stock function, which is levelname's hook (a host test installs
  levelname through `combat::InstallNormal` and asks the gate, with the key on and off).
- **boosterpool is right in arena.** It changes nothing for a name other than the suffixed booster actor. With the full level loaded, an arena call for that name
  would find no actor (only the cut level has it), so the substitution is what keeps arena from the same null component; scoping it to combat maps would leave
  that call exposed. It installs only with levelname, so a game that keeps the cut level keeps its own allocations.
- **Cost of the paths added.** `LoadLevelName`: once per level load (a handful per session), one compare. `AllocatePooledActor`: every call of every caller (twelve
  call sites in libr15, the ReVault call graph: AddBotUser, SetRemoteUserActor, AllocateMarkerActorsFromPool, FireBulletLocal, AllocateExclusive,
  SpawnBulletExclusive, SpawnMagazine, UpdateExclusive, two functions named ExclusiveUpdate and two unnamed ones). Calls per frame were not measured
  (ADR 0038 asks a measurement build; none exists). Per call the handler adds the thunk's entry (an atomic increment and two atomic loads) and, once the
  names are cached, one atomic load and one compare; before the names are cached one `process_vm_readv` and one Append call, at most a few times. Host timing
  of the handler through the real thunk, against the same call made directly: see the Host measurement below. It is an x86-64 host figure, not a Quest one.

## Host measurement

The booster pool's handler through the real thunk and the real `GotHook` slot, built `-O2` on an x86-64 host, 50,000,000 calls of a name that is not the
suffixed one, three repetitions, against the same call made directly (a stand-in for the function that returns at once; Append is a stand-in too):
2.0 to 2.3 ns for the direct call, 6.9 to 7.3 ns through the thunk and handler once the names are cached, so 4.5 to 5.2 ns added per call. The first call of a
process, which reads the plain name with `process_vm_readv` and calls Append, costs 893 ns on that host; the q15t plugin's handler paid that on every call.
These are host numbers: the Quest's cost per call and the calls per frame were not measured, so the added time per frame (ADR 0038) is not established.

## Prediction (written before any headset run)

On a combat APK built at a tag after this unit, or a development APK of the tree repacked with the `combat` variant word (or with `combat.force: true`):
1. the sentinel log has `combat_levelname_install` with `result` `installed` and `slots` 4, and `combat_boosterpool_install` with `result` `installed`, in the
   combat normal step (`combat_install` `phase` `normal`, `levelname=installed boosterpool=installed`);
2. at the first global_mp load, one `combat_levelname_kept` with `n` 1 (the lowspec bit is set on a Quest), and the archive loader is given 0x3f99..., not the
   `_lowspec` name; the match runs without the BodyTypeExclusionFlags crash;
3. every other level loads exactly as before: no `combat_levelname_kept` for it.

## Falsifiers

- `combat_levelname_install` with `result` `refused` and `reason` `original_mismatch`: a word holds another function than base + 0x1202ecc (a preempting
  definition), or the base is wrong. Nothing is left installed.
- Installed, but a global_mp load writes no `combat_levelname_kept` while the level still loads as the cut one: the call did not reach the handler (an object of a
  class outside the four vtables, or a call not through slot 0x260), or bit 31 of game+0x7ad8 was clear (then the stock was already plain).
- A crash 0.7 s into the match in `CPhysicsCS::BodyTypeExclusionFlags` with both lines present: another caller builds the suffixed name.
- A load failure of 0x3f99... after the line: the pack has no full global_mp.
- An ordinary level changing its loaded name: the handler is wrong (the host test pins this).

## Headset row

Log only for the install; for arena the cost row: on a combat APK, an arena match on a Quest 2, PSS and frame time with `combat.levelname: true` against `false`.
Not run: no headset in this unit.

## Host tests

`just test-quest-combat levelname` runs `levelname_test` (each of the four words holds its own thunk and calls its own original; the rule and each part of it;
an unreadable object; the counter and the sixteen lines; all four words or none; the refusals; the switch; the legacy file as a Warning only; a read-only vtable
page) and `combat_install_test` (the two rows, the key, the booster pool's dependency); `just test-quest-combat boosterpool` runs `boosterpool_test`.
`just test-quest-hooks-pinned` runs `got_pinned_test`, which reads the four ABS64 relocations, the stub's slot, the AllocatePooledActor and Append slots, the
plain name's address and every pinned code word in the real libr15. The mutants are `src/quest/combat/mutants.sh levelname` and `boosterpool`.
Not shown: the dynamic class of the game object on a Quest (all four words are hooked, so it does not matter); callers of the virtual slot outside the scanned
namespaces; the behaviour of the PLT stub at 0xf133d0 on the device (the host test calls a model of the plain function).
