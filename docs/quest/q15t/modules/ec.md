# Module `ec` and `ecfx`: EC rules layer (EC network client, EC token page, tac-mod store / hide, NEUTRON wall effects)

Pinned build: libr15 build id b243509c08ce677aeb95fa348016949b3fc45230 (the disassembly). Source: `<source build>/the source build's ec`,
the source build's ec_core, the source build's ecfx, the source build's ecfx_core (read-only, never edited, never pushed).

## 0. The three questions, answered before any code

### (a) What do the EC script libs need from the host, and can a plugin loaded by the Quest plugin loader supply it?

What they need (all of it, nothing else from the host):
- One exported **data** symbol, `re33_ec_page`: 4096 zero-initialised bytes, 4096-aligned. The EC script libs reach it with
  `dlsym(RTLD_DEFAULT, "re33_ec_page")` and take `page - 0x2104000` as echovr.exe's image base
  (`<source build>/the source build's tools` `XC_EC_DLSYM(n) dlsym(RTLD_DEFAULT, n)`; the guard lib template
  the source build's tools `dlsym(RTLD_DEFAULT, "re33_ec_page")`). Missing symbol is handled by the script: one
  `RE33_EC_BASE_MISSING` line and the script falls back to "a PC without the EC network client" (a source-build script). So a page that is not
  found fails safe.
- The page layout (the source build's ec_core:20-33) and the writers of it: tokens at +0xca4.., use counters, the selected tac mod at +0xe80,
  the server-has-EC flag at +0x40, the NEUTRON queue at +0xda0 and its gate at +0xe20. Those are host-side ABI and are in scope.
- Nothing is called: the host only owns the page and writes into it.

Can a plugin supply it? **The export is built, but whether the scripts can resolve it is the open question, and the evidence
leans "no, not as the loader loads it".**
- The Quest plugin loader opens every plugin with `dlopen(<native lib dir>/<file>, RTLD_NOW | RTLD_LOCAL)`
  (`src/quest/plugin/plugin_platform.cpp:71`; the only `RTLD_LOCAL`/`RTLD_GLOBAL` in
  `src/quest/plugin` and in ADR 0019, `grep -n RTLD_ docs/quest/q15t/adr/0019-quest-plugin-loader.md` finds neither word). Symbols of an
  `RTLD_LOCAL` object are not in the global lookup scope, so `dlsym(RTLD_DEFAULT, ...)` from a script lib cannot see them
  (POSIX semantics, and the host glibc test below confirms it; I have not read bionic's linker source here, so "bionic does the same for RTLD_DEFAULT" is an unverified expectation, not a finding). ADR 0019 section 4 lists the exports a Quest plugin may provide and
  none of them is a data symbol for the game's other libraries.
- The source build gets the symbol for free: the source build's ec is compiled into the runtime's `libvrapi.so`, which I **infer** is a
  DT_NEEDED of the game and so in the global scope (I did not read the game's DT_NEEDED list here), and the source build's tools adds the `re33_ec_page` symbol to its export map.
  A plugin has no such route.
- What the plugin does about it (src/quest/plugins/q15t/src/ec.cpp `PublishEcPage`). Finding, from src/quest/tests/q15t/ec_page_test.cpp run twice: a default-visibility
  *data* symbol of a plugin binds through its own GOT to an earlier global definition (ELF symbol preemption), so if another
  object exports `re33_ec_page` the plugin's own code would write that page, not its storage. The fix is built in: the storage
  object `q15t_ec_page_storage` is hidden (the plugin's own code always uses it, never the GOT) and `re33_ec_page` is a
  default-visibility **alias** of it (`__attribute__((alias))`, src/quest/plugins/q15t/src/ec.cpp:28-31), so the export exists without becoming the
  symbol the plugin's code resolves. A bare `extern` redeclaration of the symbol in another translation unit does *not* hide
  it (tested on g++ and NDK clang 17, -fvisibility=hidden); an earlier version of this note and of src/quest/plugins/q15t/src/ec.h said it did, and that
  was wrong. At install the plugin re-opens itself (`dladdr` own path,
  `dlopen(path, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL)`) to ask the linker to promote it to the global scope, then checks with
  `dlsym(RTLD_DEFAULT, "re33_ec_page")` that the result is *this* page, and logs which of three outcomes it was (visible /
  invisible / another page already exported). On glibc the promotion works and the host test shows it (own page visible:
  `state=1`; with another global page: `state=3`). **On bionic I have not verified that `RTLD_NOLOAD | RTLD_GLOBAL` promotes an
  already-loaded object; the linker source is not on this machine and I did not guess.** The log line is the answer on a headset.
- Consequence if it is not visible: scripts decide as on a PC without the EC network client (safe). The EC network client, the tac-mod store and
  the list hide do not go through the page and still work. The token/NEUTRON effects (which are the scripts' output) do not.
- The proper fix, outside this repo and outside my remit: the host (`plugin_platform.cpp:71`) opens a plugin that declares it
  needs it with `RTLD_GLOBAL`, or the sentinel (`ovrplatformloader`, DT_NEEDED of libr15, therefore global) exports the page.
  That is an nevr-runtime change; I made none.
- Two traps for the main seat: (1) a non-zero `NvrPluginInitEx` return makes the loader `dlclose` the plugin
  (`plugin_loader.cpp:201-204`, `end()` -> `close()` at :127-133) which would unmap the page and every wrapper a swapped GOT word
  points at: once any hook is installed the init must return 0 (the q15t `InitEx` already always returns 0 after installs).
  (2) If another provider already exports `re33_ec_page` (the source build's own `libvrapi.so` on a device that still carries it),
  scripts resolve that one, not ours; the plugin reports it (ERROR, `page=other`) and its libc/WriteLog hooks then refuse
  because the source build's source-build ec already owns those words (checked-swap `unexpected_original`).

### (b) Slot ownership: recvfrom / sendto / WriteLog

Slots checked against `readelf -rW` (a source-build note, not in this tree):

    :116837: 00000000036d3e88  0000007800000402 R_AARCH64_JUMP_SLOT 0000000000000000 recvfrom@LIBC + 0
    :120590: 00000000036db3d0  0000009f00000402 R_AARCH64_JUMP_SLOT 0000000000000000 sendto@LIBC + 0
    :137991: 00000000036fd398  0001499400000402 R_AARCH64_JUMP_SLOT 0000000000f88c20 _ZN10NRadEngine9NWriteLog8WriteLogENS0_9ELogLevelEyPKcz + 0
    :128202: 00000000036ea1b0  0001526800000402 R_AARCH64_JUMP_SLOT 00000000020feda8 _ZN10NRadEngine8NRadGame4NR1521ReloadEquipmentConfigEv + 0
    :132003: 00000000036f1878  0000609e00000402 R_AARCH64_JUMP_SLOT 00000000020ff7b4 _ZN10NRadEngine8NRadGame4NR1521UnloadEquipmentConfigEv + 0
    :112330: 00000000036cb1b0  00003dd500000402 R_AARCH64_JUMP_SLOT 0000000000f88720 _ZN10NRadEngine9NWriteLog9WriteLogVENS0_9ELogLevelEyPKcSt9__va_list + 0

The source's constants are right (the source build's ec:99 `kGotRecvfrom = 0x36d3e88, kGotSendto = 0x36db3d0`; :102 `kTacModList = 0x37af5f8`).
Per-slot disassembly evidence is in section 3.

Ownership, searched in every nevr-runtime tree (`src/quest`, `modules`,
`plugins`): `grep -rnIlE "recvfrom|sendto|NWriteLog|36d3e88|36db3d0|36fd398"` returned no file; the plugin-repack tree additionally
`grep -rnIiE "36d3e88|36db3d0|36fd398|37af5f8|\"recvfrom\"|\"sendto\""` over `src docs tools/*.txt tools/*.json tools/symbols.pin`:
no hit. The sentinel's pinned libc slot list (`src/quest/sentinel/pinned_targets.h`) holds one libc import, `clock_gettime`
(JUMP_SLOT 0x36c33e8, :416-418); none of mine. The only `WriteLog` strings in nevr-runtime's Quest-reachable sources are the
PC-side `EchoVR::WriteLog` ABI pointer (`src/abi/echovr_functions.cpp:120`, a PC address) and ADR text. **No ownership conflict
today.** None of this repo's other modules uses these slots either (`grep -rn "36d3e88\|36db3d0\|36fd398" src`: only mine).

Residual risks, named:
- `GotHook` (`src/quest/sentinel/got_hook.h`) enforces one owner per slot only among its own handles; the plugin's swap is
  `SwapWord`, invisible to it. If the sentinel ever hooks `recvfrom`/`sendto`/`WriteLog` it would see our wrapper in an
  executable mapping and accept it as the "original" (chain), which works, but nothing prevents a conflicting future hook. A
  slot ownership registry is nevr-runtime's to provide; I did not touch it.
- The source also swaps the WriteLog slot of `libpnsovr.so` and `libpnsrad.so` (the source build's ec:123-127, :419-448). The string
  "[NSUSER] sent profile update: %s" exists in all three libs (libr15 0x327385a, the two platform libs 0x684cc5 and 0x75320a), each with
  its own `SaveClientProfileChanges`. Which one runs on a Quest session is not known to me, so the platform libs are covered by
  the same name-lookup path the source uses, guarded by the same 25-word body check.

### (c) What needs game-derived content, and what does not

Out of scope, game content generated on the device, not produced here:
- The EC script libs themselves (the lifted EC stubs, `libsrc/*/*.cpp`, a source-build script/a source-build script output) and the wall lib
  `libac4e9374a146d3a4.so` with its four entries `re33_ecent_ac4e93_17900/_17700/_17b00/_17b60`. `ecfx` hard-codes
  that name and those entry names as the source does; they are **unverified here** and exist only if the device generated them.
- The seven tac-mod symbols and the EC tac-mod names `smoke`/`buff`/`point` are the upstream EC constants
  (the source build's ec_core:162-170, :247). They are game data, copied from the source build as constants, not re-derived.

In scope and built (host-side ABI): the page, the EC network packet and the token / dedupe / server-pick rules, the recvfrom / sendto
wrappers, the profile-update watcher and tac-mod store, the tac-mod list hide with its rebuild gate, the NEUTRON queue drain and its
gate, the install and refusal logic, the logging.

See sections 1 to 6 below for rows, mechanism, evidence, behaviour, prediction, and what is blocked.

## 1. Rows and mechanism

| Row | What | Slot / address (ELF vaddr) | Mechanism | Module |
|---|---|---|---|---|
| R41 | `NWriteLog::WriteLog` of libr15 | JUMP_SLOT 0x36fd398 (stock word = libr15 + 0xf88c20, disassembly 0x1088c20) | `InstallImportHook`/`SwapWord` of the data word to `WriteLogWrap<0>`; the wrapper is WriteLog's own 25-word body in C (`va_start`, call WriteLogV through the pinned WriteLogV word, return its result untouched) plus one look at the format string "[NSUSER] sent profile update: %s" | ec |
| R42 | tac-mod list pointer | .bss word 0x37af5f8 (disassembly 0x38af5f8) | not a hook: read with SafeRead under the rebuild gate; the count field is lowered/restored by compare-and-swap when the server lacks EC | ec |
| R43 | libc `recvfrom` | JUMP_SLOT 0x36d3e88 | data-word swap to `RecvfromWrap`: consumes our 16-byte datagrams, retries the real call; hosts the net tick | ec |
| R44 | libc `sendto` | JUMP_SLOT 0x36db3d0 | data-word swap to `SendtoWrap`: counts destinations (server pick) | ec |
| gate | `ReloadEquipmentConfig` / `UnloadEquipmentConfig` | JUMP_SLOT 0x36ea1b0 (fn 0x20feda8) / 0x36f1878 (fn 0x20ff7b4) | data-word swaps to `GearWrap<0/1>` which bump a generation and hold a lock around the real call, so the list pointer is never read mid-rebuild | ec |
| (platform libs) | `WriteLog` of libpnsovr.so / libpnsrad.so | found by name through the lib's dynamic table (`JumpSlot`), not pinned | the same wrapper, only when the word is a WriteLog of the known 25-word body and its owner has a WriteLogV word that holds WriteLogV | ec |
| export | `re33_ec_page` | storage `q15t_ec_page_storage` (hidden) plus exported alias | see (a) | ec |
| ecfx | NEUTRON queue drain | page +0xda0 (6 x {ctx, word}), gate +0xe20 | no GOT word: registered on ec's net tick (`EcOnNetTick`, 4 slots), gate opened only after the first drain ran | ecfx |

recvfrom and sendto and the three WriteLog words (libr15, pnsovr, pnsrad) and the two equipment slots are all **data-page** writes;
no code page is written (execmod is forbidden). Install is both-or-none for the net pair (`InstallNet`) and for the gate pair
(`InstallGearGate`): if the second swap is refused the first is undone (`Undo`), and every refusal is a logged ERROR/WARNING.
The selection watcher is separate and independent of both.

## 2. Source-build evidence (read-only, never edited)

Files: `<source build>/the source build's ec` (slots :99-102, platform libs :123-127 and :419-448, Main thread),
the source build's ec_core (page layout :20-33, token types, tac-mod rules :162-170 and :247, hide rule), the source build's ecfx / the source build's ecfx_core
(queue, gate). The pure rules in `src/quest/plugins/q15t/src/ec_core.h` and `src/quest/plugins/q15t/src/ecfx_core.h` are the ports of the `*_core.h` files; the differences are
deliberate hardenings (everything the game owns goes through `SafeRead`; the list count is written by CAS; datagram length and
`from` family are checked; one `recvfrom` call never swallows more than 256 datagrams in a row; no namespace-scope constructors).

## 3. disassembly evidence (libr15.so, read-only; recipe in docs/MODULE_CONTRACT.md; VA = ELF + 0x100000)

WriteLog body (R41), `functions`: `1088c20 | 100 | WriteLog`, `1088720 | 1280 | WriteLogV`. Instructions 0x1088c20..0x1088c80 (25 words;
the row at 0x1088c84 is the next function's `adrp`). Quoted as read:

    1088c20 | ff0304d1 | sub  | sp, sp, #0x100          1088c4c | e00700ad | stp | q0, q1, [sp]
    1088c24 | e8048092 | mov  | x8, #-0x28              1088c50 | e18346ad | ldp | q1, q0, [sp, #0xd0]
    1088c28 | e9030091 | mov  | x9, sp                  1088c54 | e39308a9 | stp | x3, x4, [sp, #0x88]
    1088c2c | ea230291 | add  | x10, sp, #0x88          1088c58 | e3c30291 | add | x3, sp, #0xb0
    1088c30 | 08f0dff2 | movk | x8, #0xff80, LSL #32   1088c5c | e59b09a9 | stp | x5, x6, [sp, #0x98]
    1088c34 | eb030491 | add  | x11, sp, #0x100         1088c60 | e75700f9 | str | x7, [sp, #0xa8]
    1088c38 | 29010291 | add  | x9, x9, #0x80           1088c64 | e20f01ad | stp | q2, q3, [sp, #0x20]
    1088c3c | 4aa10091 | add  | x10, x10, #0x28         1088c68 | e41702ad | stp | q4, q5, [sp, #0x40]
    1088c40 | e8fb0ea9 | stp  | x8, x30, [sp, #0xe8]    1088c6c | e61f03ad | stp | q6, q7, [sp, #0x60]
    1088c44 | eaa70da9 | stp  | x10, x9, [sp, #0xd8]    1088c70 | e18305ad | stp | q1, q0, [sp, #0xb0]
    1088c48 | eb6b00f9 | str  | x11, [sp, #0xd0]        1088c74 | 477afd97 | bl  | 0x00fe7590
                                                        1088c78 | fe7b40f9 | ldr | x30, [sp, #0xf0]
                                                        1088c7c | ff030491 | add | sp, sp, #0x100
                                                        1088c80 | c0035fd6 | ret

`0xfe7590` is `thunk_FUN_01088720` (size 16), words `303f0190 11da40f9 10c20691 20021fd6` = `adrp x16, 0x37cb000; ldr x17, [x16, #0x1b0];
add; br x17`: it goes through the slot ELF 0x36cb1b0, which readelf shows as the JUMP_SLOT of WriteLogV (value 0xf88720). That is
why the wrapper finds WriteLogV by reading that slot (and refuses if it does not hold libr15 + 0xf88720). The `kWriteLogBody`
constant in src/quest/plugins/q15t/src/ec_core.h is these 25 words; src/quest/tests/q15t/ec_test.cpp feeds the disassembly bytes into `IsWriteLogBody`.

Slots through the PLT stubs (`instructions`, `functions`):

    ff8f40  thunk_EXT_FUN_038d33a8 (16)  adrp x16,0x36d3000 ; ldr x17,[x16,#0xe88]   -> ELF slot 0x36d3e88 recvfrom   (R43)
    10079d0 thunk_EXT_FUN_038d34e0 (16)  adrp x16,0x36db000 ; ldr x17,[x16,#0x3d0]   -> ELF slot 0x36db3d0 sendto     (R44)
    104b960 thunk_FUN_01088c20     (16)  adrp x16,0x37fd000 ; ldr x17,[x16,#0x398]   -> ELF slot 0x36fd398 WriteLog   (R41)

`xrefs` to the GOT words (disassembly 0x37d3e88 / 0x37db3d0 / 0x37fd398, i.e. ELF + 0x100000): one read each, from `ff8f44`, `10079d4`,
`104b964`: only the PLT stubs read the slots, so a data swap of the word catches every caller. Callers of the stubs: recvfrom stub `ff8f40`: `1099998 ReceiveFrom`,
`112dbdc FUN_0102da38`; sendto stub `10079d0`: `10997ec SendTo` and nine calls in the `112e6b8..112ec78` range (unnamed). The
slots' relocations (`readelf -rW`) are quoted in section 0 (b); the disassembly row for `libr15.so`: sha256
`8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20`, architecture aarch64, base_address 1048576 (the disassembly `binaries`
table has no build_id column; the build id b243509c... is the contract's pin, not read from the disassembly here).

The profile-update line: the string addresses are libr15 0x327385a and the two platform libs 0x684cc5 and 0x75320a, each "[NSUSER] sent profile update: %s". In libr15 it
is referenced from `SaveClientProfileChanges` (0x1a331f8) at `1a33480` (`adrp x2, 0x3273000; add x2, x2, #0x85a`), whose call is

    1a33470 mov x0, sp ; 1a33474 bl 0x01003b60 ; 1a33478 mov x21, x0 ; 1a3347c adrp x2, 0x3273000 ; 1a33480 add x2, x2, #0x85a
    1a33484 mov w0, #0x2 ; 1a33488 mov x1, xzr ; 1a3348c mov x3, x21 ; 1a33490 bl 0x0104b960   (the WriteLog stub)

so the logged argument is the profile json, level 2, and it goes through the R41 slot. The same string exists in the two platform libs (which is pnsovr and which pnsrad I did not resolve) at `SaveClientProfileChanges`
0x48340c (the first) and 0x4cabc8 (the second): unverified that those two libs run on a Quest session.

Tac-mod list word (R42), disassembly 0x38af5f8: five xrefs: `21fecb0 read LoadEquipmentConfig`, `21fee0c data ReloadEquipmentConfig`,
`21ff020 read` and `21ff8ec read` (both unnamed), `21ffa74 write` (unnamed). `LoadEquipmentConfig` reads it as `adrp x8, 0x38af000;
add x8, x8, #0x5e8; ldp x9, x10, [x8]; ldr x8, [x8, #0x10]` (0x38af5e8 + 0x10 = the word). `ReloadEquipmentConfig` (0x21feda8, size 180) and
`UnloadEquipmentConfig` (0x21ff7b4, size 84) are the two rebuild entries whose JUMP_SLOTs (section 0 (b)) the gate wraps. **Unverified
here:** the field layout of the list object `{alloc, entries, count, cap}` and the 0x88-byte entry size are the source build's
(the source build's ec_core), not re-read in the disassembly; the code refuses any list whose capacity/count are implausible and re-checks the three
last entry names (smoke / buff / point) before it writes anything.

## 4. Behaviour

- `re33_ec_page` exists always (zeroed page: every EC script decides as on a PC without the EC network client). Nothing else happens with no
  switch (`ec.on`, `ec_preset1.on`, `ec_preset2.on`, `ec_preset3.on` in the plugin data directory; or `Q15T_EC_DEFAULT_WORDS` at build time):
  one INFO "ec: off" line and no word swapped. `ec.off` in the plugin data directory is the kill file: it keeps ec off even when the
  build has default words or `ec.on` / a preset file is there (one INFO "ec: off" line naming `kill_file=ec.off`, no word swapped).
- Switch on: the page line is logged; recvfrom/sendto are swapped (both or none). The main thread ticks every 5 ms: the use counters
  on the page become 16-byte EC network packets sent twice to the server (the busiest sendto destination of the last second); 'H' hello
  every 2 s; the server's 'A' answer (only from the current server, within 6000 ms) sets the page's has-EC flag; 'T'/'V'/'P'/'O' tokens
  and 'K' drone-kill / 'M' packets are taken from any sender, de-duplicated by id (ring of 32). Our datagrams are never handed to
  the game.
- Tac mods: the WriteLog wrapper reads the profile json's `ability`; a known EC tac mod symbol is stored at +0xe80 (with the
  `equip` word; `noequip` stores nothing). With the `hide` word and no answer from the server, once the selection has been still for
  2000 ms the list's count is lowered by 3 (smoke / buff / point hidden); it is put back when the server answers, and it is never
  cut while one of ours is selected. Reload/Unload bump a generation so a rebuilt list restarts the remembered full count.
- ecfx (needs `neutronfx.on` in the data directory): once the wall lib `libac4e9374a146d3a4.so` is loaded (checked once a second,
  quietly), its four entries are resolved and re-checked by dladdr before each call; on ec's net tick the queue is drained
  (one entry per ctx, lowest kind first; BOOM never run; a repeated sound inside 3000 ms refused; a queue not drained for over
  500 ms freed unrun), and the gate +0xe20 opens only after the first drain.
- Logging: every install, every refusal, every state change is one structured line through `Log`; capped per-kind counters and a
  10 s summary, no per-datagram lines. Examples are in the host test output; a real headset record is not produced yet (section 5).

## 5. Prediction (written before any headset run)

On a Quest with this plugin, `ec.on` present, the session on a server that runs EC:
1. Log: `ec: page published (...)` INFO at install. **Falsified by** `ec: page NOT visible` (WARNING; then bionic did not honour
   `RTLD_NOLOAD | RTLD_GLOBAL` and the scripts decide as on a PC without the EC network client) or `another re33_ec_page is already global` (ERROR).
2. `ec: server (the game's own connection)` once the game talks, then `ec: answer seen: the server has EC` and the page's
   server-has-EC flag (+0x40) at 1 within a few seconds (`ec: answer lost: no 'A' for 6 s` when it stops). **Falsified by** no server line (the sendto/recvfrom words are not the ones the game uses) or by an
   `unexpected_original` refusal on a slot (the stock word differs from what the disassembly says).
3. A tac-mod selection in the menu produces an `ec: sel` line with `result=stored at +e80`. **Falsified by** silence after choosing a mod: then
   the profile-update line is logged by a platform lib whose slot did not install (the log says `ec: sel refused` for it) or by a
   path that does not go through WriteLog.
4. On a server without EC, after 2 s of a still selection the list loses three entries (`effect=hidden`) and gets them back on an
   answer (`effect=shown`). **Falsified by** a crash in the menu (the entry layout is not the source's) or no transition.
5. ecfx: `neutronfx: waiting` once, then `neutronfx: on` and `gate +e20 = 1` after an EC level loads. **Falsified by** `OFF: the wall
   lib has no such entry` (names differ on this device: expected, they are unverified).

## 6. Word patches, blocked parts, open questions

**Needs a word patch: none.** Every row is a GOT/JUMP_SLOT data-page swap (SwapWord, checked: the word must hold the stock value
before it is replaced); no code word is read-modify-written. Stock words, as `readelf` and the stubs show: slot 0x36d3e88 and 0x36db3d0
hold libc's recvfrom/sendto (resolved at load, no libr15-relative value to check; the swap refuses if the word is not in an executable
mapping), slot 0x36fd398 holds libr15 + 0xf88c20 (checked), 0x36cb1b0 holds libr15 + 0xf88720 (checked before use),
0x36ea1b0 / 0x36f1878 hold libr15 + 0x20feda8 / + 0x20ff7b4 (checked).

Not built / blocked:
- The EC script libs: game content generated on the device (out of scope).
- The wall lib content (`libac4e9374a146d3a4.so` and its entry names): unverified; `ecfx` is tested only against a stand-in lib.
- The bionic behaviour of `dlopen(RTLD_NOLOAD | RTLD_GLOBAL)` promotion: unverified; the fix that does not depend on it is host-side
  (`RTLD_GLOBAL` for a plugin that asks, or a sentinel export): not mine to edit.
- Which of the three libs logs the profile-update line on a real Quest, and the pnsovr/pnsrad ELF identity: not
  resolved; their 25-word WriteLog bodies were not read in the disassembly (the platform path is guarded by the body check instead).

For the main seat (no shared file was edited by me): add `src/quest/plugins/q15t/src/ec.cpp src/quest/plugins/q15t/src/ecfx.cpp` to CMakeLists; read the option(s) `Q15T_EC` /
`Q15T_EC_DEFAULT_WORDS`; call `InstallEc(base)` and then `InstallEcfx(base)` from InitEx after the existing hooks; **InitEx must
return 0** whatever those return (a non-zero return makes the loader dlclose the plugin, which is fatal once a word is swapped).
