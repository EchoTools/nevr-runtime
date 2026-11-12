# Combat module `ec` and `ecfx`: the EC network client, EC's spare page, the tac-mod store and hide, NEUTRON effects

Code: `src/quest/combat/ec.{h,cpp}`, `ec_core.h`, `ec_targets.h`, the rules shared with PC `src/runtime/patch/ec_rules.h` (#2358), `ecfx.{h,cpp}`, `ecfx_core.h` (the drain's rules are `src/runtime/patch/ecfx_rules.h`, shared with PC); the page `src/quest/sentinel/ec_page.{h,cpp}`.
Tests: `src/quest/tests/ec_test.cpp`, `ec_page_test.cpp` (with `ec_page_shim.cpp`, `ec_page_libr15.cpp`, `ec_page_script.cpp`, `ec_page_other.cpp`),
`ecfx_test.cpp` (with `ecfx_wall_stub.cpp`), the `CheckRealEc` part of `got_pinned_test.cpp`, `TestExportAllowlist` and
`TestEcPageIsAnExportedDataPage` in `tests/quest/elf_groundtruth_test.go`. Mutants: `src/quest/combat/mutants.d/ec.sh`, `ecfx.sh`.
Registration: `ec`, then `ecfx`, in the normal phase of `combat_install.cpp` (after `refresh_rate`, before `gates`). Pinned build: the store
libr15 (build ID `b243509c08ce677aeb95fa348016949b3fc45230`), libpnsovr.so (`ca47bb8d...`) and libpnsrad.so (`c58fb82e...`).

## What the EC script libraries need from the runtime

One exported data symbol, `re33_ec_page`: 4096 zeroed bytes, 4096-aligned. The EC script libraries find it with
`dlsym(RTLD_DEFAULT, "re33_ec_page")` and take `page - 0x2104000` as echovr.exe's image base, so every PC offset keeps its meaning. A page
nobody writes is "a PC without the EC network client" to every EC script; a page that is not found is the same (the script logs
`RE33_EC_BASE_MISSING` and decides that way). The runtime only owns the page and writes into it; nothing in it is called.

The page layout (reference build afk_ec_core.h:20-33): token blocks (+00 count, +08 expiry, +10 shown) at +0xca8 (overcharge), +0xcc0 (veil),
+0xce0 (point), +0xd20 (translocator); use counters at +0xca4, +0xd40, +0xf00, +0xf80; the drone block at +0xd60; the selected tac mod at +0xe80
(+0xe90 skips, +0xea0 re-runs); the NEUTRON queue at +0xda0 and its gate at +0xe20; and the Quest-only word +0x40, 1 while the current server
answers the EC network.

### The export

`sentinel/ec_page.cpp` is a source of `libovrplatformloader.so` itself, not of the combat archive: the sentinel links its archives with
`-Wl,--exclude-libs,ALL`, which hides every symbol of an archive member. The storage `nevr_ec_page_storage` is hidden and `re33_ec_page` is a
default-visibility alias of it. A default-visibility data symbol is preemptible, so if another object already exports `re33_ec_page` in the
global scope, a reference to it from inside the sentinel would bind to that page through the GOT; the hidden storage binds locally, and the
runtime's code reaches the page through `sentinel::EcPage()` only. `ec_page_test_other` loads another exporter first and shows the sentinel
still writes its own storage.

At install, `PublishEcPage` re-opens the sentinel's own path with `RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL` (the extra reference is kept for the
process) and asks `dlsym(RTLD_DEFAULT)`; the `combat_ec_page` line says which of three it got (`visible`, `invisible`, `other`).

What is shown and what is not:
- Shown on the built arm64 sentinel: `re33_ec_page` is a dynamic export, 0x1000 bytes in .bss, 4096-aligned
  (`TestEcPageIsAnExportedDataPage`), and the export list is exactly `nevr_sentinel_marker`, `JNI_OnLoad` and `re33_ec_page` (`TestExportAllowlist`).
- Shown on glibc (`ec_page_test`): a process that loads a stand-in for libr15 RTLD_LOCAL, with the sentinel's page object as its DT_NEEDED,
  does not resolve the page from a third library's `dlsym(RTLD_DEFAULT)` before the promotion and resolves the sentinel's storage after it.
- Not shown: bionic. The sentinel is a DT_NEEDED of libr15, which the app loads; bionic's `dlsym(RTLD_DEFAULT)` searches the global group and then
  the caller's local group, so a script library that has libr15 or the sentinel among its DT_NEEDED would find the page without the promotion
  (the store's lobby script `libf808c072bb49da47.so` lists `libovrplatformloader.so` as NEEDED; `lib555db38933fef859.so` lists only libdl and
  libc), and whether `RTLD_NOLOAD | RTLD_GLOBAL` promotes an object already loaded is not measured. The promotion is kept. The `combat_ec_page`
  line on a headset, and an EC script's own `RE33_EC_BASE_MISSING` line, are the answer.

## Slots

`readelf -rW` of the store files; `got_pinned_test` (`CheckRealEc`) checks each relocation, each function's `.dynsym` value and every pinned word.

| Hook | Module | Slot (ELF vaddr) | Holds | Handler |
| --- | --- | --- | --- | --- |
| recvfrom | libr15 | JUMP_SLOT 0x36d3e88 | libc's recvfrom (plausibility only) | consumes the EC network's datagrams; the net tick |
| sendto | libr15 | JUMP_SLOT 0x36db3d0 | libc's sendto (plausibility only) | counts destinations (the server pick) |
| writelogv | libr15 | JUMP_SLOT 0x36cb1b0 | base + 0xf88720 | the selected tac mod |
| reload_equipment_config | libr15 | JUMP_SLOT 0x36ea1b0 | base + 0x20feda8 | the hide's gate |
| unload_equipment_config | libr15 | JUMP_SLOT 0x36f1878 | base + 0x20ff7b4 | the hide's gate |
| writelogv | libpnsovr.so | JUMP_SLOT 0x6e1238 | its own WriteLogV (0x1ca79c) or libr15's | the selected tac mod |
| writelogv | libpnsrad.so | JUMP_SLOT 0x731ee8 | its own WriteLogV (0x1be7b0) or libr15's | the selected tac mod |

The tac-mod list pointer is the .bss word 0x37af5f8, read only (never hooked), under the gate.

### Why WriteLogV and not WriteLog

`NWriteLog::WriteLog(ELogLevel, unsigned long long, const char*, ...)` is variadic. A `CallbackThunk` forwards only its named arguments, so a
thunk on WriteLog's slot would cut the arguments off every log line of the game. In each of the three libraries WriteLog is 25 words (libr15
0xf88c20, libpnsovr 0x1cac9c, libpnsrad 0x1becb0; `llvm-objdump -d` of the store files): it stores x3..x7 and q0..q7 in a save area, builds the
va_list at sp+0xb0, puts its address in x3, calls WriteLogV through its own PLT stub with x0..x2 untouched, and returns what WriteLogV left in x0.
The stub (`adrp x16; ldr x17,[x16,#lo]; add x16,x16,#lo; br x17`) reads the WriteLogV slot in the table above. A thunk of
`uintptr_t(int, unsigned long long, const char*, va_list)` on that slot sees every WriteLog call with its own arguments, and passes the va_list on
as the ABI does: AAPCS64 passes the 32-byte `__va_list` by reference, so the callee gets a pointer in x3 as it does unhooked. In libr15 every
`adrp` of the slot's GOT page (512 of page 0x36cb000) is an `adrp x16` of the PLT, and the WriteLogV stub 0xee7590 has exactly two callers:
0xf88710 in `WriteLog(ELogLevel, const char*, ...)` and 0xf88c74 in `WriteLog(ELogLevel, unsigned long long, const char*, ...)`. So nothing a
WriteLog-slot hook would see is missed, and the one further caller is the other WriteLog overload, which is a log line too. The install checks
the 25 words (each library's own `bl`) and the stub's four words before it hooks; a word that differs refuses that library's hook
(`writelog_body_not_known`, `writelogv_stub_not_known`) and leaves the net pair in.

The combat plugin imported in #1591 hooked WriteLog itself and rebuilt the va_list in a C wrapper that called the owner's WriteLogV; an assembly entry that keeps
x0..x7, v0..v7 and the stack and `br`s on was the other choice. The WriteLogV thunk needs neither and the frame sensor checks it like any thunk.

### The platform libraries

libpnsovr.so and libpnsrad.so are loaded by libr15 after the combat step (CNSProvider::Create -> CSysModule::Load -> dlopen). Their WriteLogV
hooks go in from the module-loaded observer (`integration/post_load.h`: a table of four observers, the crash-first hook's in the first slot,
`AddModuleLoadedObserver` for the others), which the dlopen hook calls for every non-null handle; ec adds its observer at install and hooks a
library already loaded at install too. A library mapped again (another base) is hooked again: the hook left from the old mapping is forgotten
first. The platform library's slot may hold its own WriteLogV or libr15's (whichever the loader bound); any other value is refused
(`slot_not_writelogv`). One `combat_ec_platform` line per change of outcome.

## Behaviour

- **The net pair**, both or none (a refused sendto removes the recvfrom hook). A datagram of exactly 16 bytes with both magics ("ECVL", "VEIL")
  and a known type (every type EchoCombat v2.0.0's wire carries: H A V P T O K M and match control's C L R G), not peeked, is the EC
  network's and is not handed to the game: recvfrom is called again with the same arguments (a non-blocking socket gives the game the next
  datagram or EAGAIN). One call swallows at most 256 in a row. 'K', 'M' and the carried types come from any sender, de-duplicated by id
  (a ring of 32, which an 'M' takes a slot of, as on PC); 'A' (a server of an older build answering the hello) and a first-time 'M' are
  counted and open nothing; C L R G are taken out and ignored. These rules are `src/runtime/patch/ec_rules.h`, which PC's
  `src/runtime/combat/veilnet.cpp` follows too.
- **The server's statement** (#2333): `CR15NetGame::LobbySettingsCB(SMsg)` (its GLOB_DAT word, which `BeginMultiplayer` registers) reads the
  `nevr_server` key of the lobby settings the server gives its clients (`src/runtime/compat/server_features.h`). A server that states
  `echocombat` opens the gate the tac-mod hide follows; any other document, or none, keeps it closed. A session in another lobby clears it
  (`src/runtime/compat/session_router.cpp`).
- **The thread**, on a combat map or the combat lobby every 5 ms: the use counters become 'V' / 'P' / 'T' / 'O' packets sent twice to the server
  (the busiest sendto destination of the last second), the hello 'H' every 2 s (the server relays only to clients that said hello), the page's +0x40 word (the server states EchoCombat), the summary every 10 s.
- **The selected tac mod**: the line `[NSUSER] sent profile update: %s` carries the profile json; its `ability` names the tac mod, and a known EC
  one is stored at +0xe80 (with `equip`; `noequip` logs only).
- **The hide** (with `hide`, the gear gate in): on a server that does not answer, once the selection has been still for 2000 ms, the tac-mod list's
  count is lowered by 3 (smoke, buff, point are the list's last three, re-checked by name before any write; the count is written by
  compare-and-swap); it is put back when the server answers, and never cut while one of the three is selected. Reload and Unload of the
  equipment config hold the gate and bump a generation, so a rebuilt list starts again.
- **The net tick**: the end of every recvfrom on the game's own socket, on the thread that received a datagram of the current server on it,
  calls up to four callbacks (`ec::OnNetTick`): ecfx and powerboost hang there. A first-time 'K' goes to `ec::OnDroneKilled`'s handler (dronekill).
- **ecfx**: once the wall library `libac4e9374a146d3a4.so` is loaded (looked for once a second by its own thread, quietly), its four entries
  `re33_ecent_ac4e93_17900` / `_17700` / `_17b00` / `_17b60` are resolved and re-checked by `dladdr` (name and exact start) before each call; on
  the net tick the queue is drained (one entry per ctx, the lowest kind first, each run as the drain reaches its slot and its slot freed after
  it returns; BOOM is the server's and is never run; a repeated sound inside 3000 ms, a sound whose wall cannot be read, an entry of 0 or with
  bit 2 or 3 set, and an entry that is not the wall library's for its kind are freed unrun; a queue not drained for over 500 ms is freed
  unrun), and the gate +0xe20 opens only after the first drain. The rules are `src/runtime/patch/ecfx_rules.h`, which PC's
  `src/runtime/combat/dronefix.cpp` drains through too, with the exact-entry check off (#2349).

## Arena

Arena and combat run on the same APK, so every hook above is installed while a player is in an arena match. The scope is `ec::InCombatScope()`:
an atomic the ec thread refreshes at most every 250 ms from one record of the combat level answer (`combat_scope.h` `LastLook()`, read whole on
that thread, judged by `InCombatOrLobby(look)`), true only while a combat map (`mpl_combat_*`) or the combat lobby (`mpl_lobby_b_combat`) is loaded
and the answer is fresh. A combat answer older than 1 s (the game thread stopped refreshing it) is out: the client leaves the scope until the tick is
back, and its line names the class `stale`. The bound: a stopped tick takes the client out of scope within 1 s plus 250 ms (plus one 5 ms step) of the
tick's last look, because the freshness test is made at the client's own looks, at most 250 ms apart in scope (`ec.cpp` `kScopeEveryMs`, `kStepUs`). A torn read of the answer (no whole record in four tries) is named `no_look` and takes the client out of
scope until the next read, 250 ms later (it fails closed). A level change reaches the client within about 500 ms: the game thread's look is made at most
every 250 ms and this thread reads at most every 250 ms, plus up to one game frame. The module is one of the scope row's readers (`combat.ec` is in `ScopeNormal()` in `combat_install.cpp`),
and its install refuses with `scope_not_live` when the answer cannot change. An Echo Arena level is out, and so are the
lobbies arena players use (`mpl_lobby_b`, `_b2`, `_b2_lowspec`, `_b_arena`, `_b_social`), which are no Echo Arena level. Out of scope:

- recvfrom and sendto call libc and return: no datagram is consumed, nothing is counted, no lock is taken, no net tick runs (so ecfx never drains
  and powerboost never sends there);
- the thread sleeps 250 ms between looks and sends nothing (no hello, no use packet); leaving the scope clears the page's +0x40 word (and the
  step's record of it, so the first step back writes it again on a server that states EchoCombat) and the game socket the net tick follows.

Two parts act in every level, on purpose: the WriteLogV handler stores a selected tac mod into re33_ec_page (memory of ours; a selection made in a
menu must reach the combat lobby) and writes its capped `combat_ec_sel` line, and the gear gate takes its mutex around ReloadEquipmentConfig /
UnloadEquipmentConfig (an event, not a frame) so a rebuild that began before the scope changed is never read half-built.

Outside a combat map and the combat lobby, ec sends no datagram and takes no lock on the socket calls. It also keeps its own scope narrower than
kotorvr's echoquest-combat, which pauses its EC client only while an Echo Arena level is loaded. In the lobbies arena players use (`mpl_lobby_b`, `_b2`,
`_b2_lowspec`, `_b_arena`, `_b_social`) the EC client does not run, so `hide` does not cut the tac-mod list (smoke, buff and point) there, as it does in the
combat lobby and on a combat map (on a server without EC). Which lobby a Quest combat player chooses a loadout in is not established; the headset row below
has a line for it.
The cost per call is in the unit's arena notes and
the PR body (ADR 0038).

## Config

| Key | Default | Meaning |
| --- | --- | --- |
| `combat.ec` | true | The EC client: the page writer, the net pair, the thread, the tac-mod store and hide. |
| `combat.ec_words` | absent: `types=VPTO equip hide` | `types=<letters of VPTO>` (the types carried; none, empty or another letter: 'T' only), `equip` / `noequip`, `hide`, `loctoken=<s>` (TEST: a LOC token every s seconds, 1..600). |
| `combat.ecfx` | true | NEUTRON effects on ec's net tick (needs ec). |

The defaults are echoquest-combat's (the reference build's on-list ships `ecplayo.on` = `types=VPTO equip hide` and `neutronfx.on`), as the combat
plugin imported in #1591 shipped them. The plugin's files `ec.on`, `ec.off`, `ec_preset1.on` .. `ec_preset3.on` and `neutronfx.on` are not read; one found in the
plugin's data directory is a Warning (`combat_ec_legacy_switch`, `combat_ecfx_legacy_switch`) that names the key.

## Lines (sentinel log)

    {"event":"combat_ec_install","result":"installed|partial|refused|off","reason":"..","net":"..","gear_gate":"..","writelogv_libr15":"..","writelogv_pnsovr":"..","writelogv_pnsrad":"..","consequence":".."}
    {"event":"combat_ec_hook","hook":"recvfrom|sendto|lobby_settings_cb|writelogv|reload_equipment_config|unload_equipment_config","module":"..","symbol":"..","status":".."}
    {"event":"combat_ec_lobby_settings","statement":"stated|not_stated|malformed","features":"..","dropped":<n>,"bytes":<n>,"changed":0|1,"msg":".."}
    {"event":"combat_ec_page","page":"0x..","global_lookup":"0x..","reopened_global":0|1,"state":"visible|invisible|other","msg":".."}
    {"event":"combat_ec_words","source":"default|config","types_mask":"0x..","hide_write":0|1,"equip_store":0|1,"loctoken_s":<n>}
    {"event":"combat_ec_scope","in_scope":0|1,"level_class":"combat_or_lobby|echo_arena|other|no_look|stale","level":"0x..","msg":".."}   (combat_scope.h LookClassName; level: the look's)
    {"event":"combat_ec_platform","lib":"libpnsovr|libpnsrad","status":"..","msg":".."}
    {"event":"combat_ec_server","addr":"..","port":<n>,"fd":<n>}   {"event":"combat_ec_server_features","state":"stated|not_stated"}
    combat_ec_used, combat_ec_token, combat_ec_kill, combat_ec_sel, combat_ec_tacmods, combat_ec_skip, combat_ec_prelude, combat_ec_loc, combat_ec_wall
    (48 each per process, kill 16), combat_ec_sum (every 10 s, 60 in all), combat_ec_legacy_switch (Warning)
    combat_ecfx_install, combat_ecfx_waiting (once), combat_ecfx_attach, combat_ecfx_gate (once), combat_ecfx_ran / combat_ecfx_drain (the first 96),
    combat_ecfx_lines (once: later ones are counted), combat_ecfx_sum (the counts, at most every 10 s and after 10 s quiet)

## Headset row

On a combat APK built at a tag after #2187 (or a development APK of the tree repacked with the `combat` variant word):
`combat_ec_install` with the six libr15 hooks by name `installed` and `combat_ec_platform` `installed` for libpnsovr and libpnsrad once they load;
`combat_ec_page` `visible` (`ec: page published`); `combat_ec_scope` `in_scope:1` on a combat map and `in_scope:0` in an arena match; in a combat
match on a server that states EchoCombat, `combat_ec_lobby_settings` `stated` with `features` naming `echocombat`, then `combat_ec_server` and
`combat_ec_server_features` `stated`; with the wall library, `combat_ecfx_attach`
`on` and `combat_ecfx_gate`. **Falsified by** `combat_ec_install` not `installed`/`partial`, `combat_ec_page` `invisible` or `other` (or an EC
script's `RE33_EC_BASE_MISSING`), no `combat_ec_server_features` `stated` on a server whose registration carries `echocombat`, or any EC datagram (`combat_ec_used`, a hello) while
`in_scope` is 0.

Lobby row: in a shared lobby (`mpl_lobby_b`, `_b2`, `_b_social`, `_b_arena`) `combat_ec_scope` `in_scope:0`, no `combat_ec_used` and no hello; and, on a
combat player's return to the lobby they choose a loadout in, whether the tac-mod list is whole there (it is expected whole in a shared lobby and cut in
`mpl_lobby_b_combat` on a server without EC). **Falsified by** a datagram with `in_scope:0`, or a tac-mod list cut in a shared lobby. If the combat player's
loadout lobby is a shared one, the scope is narrower than upstream where it shows; widen it only on that evidence.

The page's visibility to the script libraries is not shown by `state`: `PublishEcPage` asks from inside the sentinel, whose own local group may find the
export whether or not the `RTLD_GLOBAL` promotion took. The cheap on-device probe: from the publish step call bionic's `__loader_dlsym(RTLD_DEFAULT,
"re33_ec_page", <an address inside a loaded script library>)` (the caller address a script's own `dlsym` passes) and log the result next to `state`; the
row is met when it returns the page. Not built yet (#2313).

## Not built here

The EC script libraries and the wall library are game content generated on the device; their names and entry names are unverified here, and ecfx
is tested only against a stand-in library. The tac-mod list's layout `{allocator, entries, count, capacity}` and its 0x88-byte entries are the
reference build's; the code refuses an implausible list and re-checks the three names before it writes.

## Origin

The combat plugin's `ec.cpp`, `ec_core.h`, `ecfx.cpp`, `ecfx_core.h` (imported in #1591), ports of the reference build's runtime/native/afkxr/afk_ec.cpp,
afk_ec_core.h, afk_ecfx.cpp, afk_ecfx_core.h (pinned `1357409` in docs/audits/2026-11-05-q15t-origin-citations.md rows 53-62, 188-211).
The EC network rules are EchoCombat v2.0.0's VeilNet (plugin/veilnet.cpp), shared with PC in `src/runtime/patch/ec_rules.h`. The NEUTRON
deferral is its DroneFix (plugin/dronefix.cpp L332-409, L682-689, L1683-1686), EchoCombat `c14c3e0a`.
