# Module `peerclamp` (row R36)

Files: `src/quest/combat/peerclamp.{h,cpp}`, `src/quest/combat/peerclamp_targets.h` (the pins), `src/quest/combat/combat_scope.{h,cpp}` (the combat level answer it is
scoped by), `src/quest/tests/peerclamp_test.cpp`, `src/quest/combat/mutants.d/peerclamp.sh`. The module is a normal-phase row of the combat install table
(`src/quest/combat/combat_install.h`): it runs in the combat APKs and in no other. Switch in `config.yaml` (docs/adr/0046 rule 4):
`combat.peerclamp` (default true). There is no switch file for it.

Origin: Ducky's echoquest-combat reference build, afk_peerclamp.cpp (the rule at :1, the constants at :39-44, the 26 pinned words at :50-62, the clamp at
:83-116; docs/audits/2026-11-05-q15t-origin-citations.md rows 99-101 and 241-242), reconstructed in this tree (imported in #1591). Upstream
it was test only; the plugin built it on (#1625). The scope is the runtime's (`combat_scope.h`, #2246).

## Row covered

R36: `SBroadcasterData::CleanupPeers()`, JUMP_SLOT ELF 0x36efad8, real function ELF 0x24ef9b0 (disassembly 0x25ef9b0, 1260 bytes). `readelf -rW` of the store libr15
(sha256 `8dd9a961...`, build ID `b243509c...`) lists `00000000036efad8  R_AARCH64_JUMP_SLOT  00000000024ef9b0 _ZN10NRadEngine16SBroadcasterData12CleanupPeersEv`.
Its one caller is `CBroadcaster::Receive(uint)` (`ldr x0,[x19]; bl <PLT CleanupPeers>` at 0x24eeec8). The words the walk depends on (the whole table is
`kWords` in `peerclamp_targets.h`):

```
24ef9d8 ldr w27, [x0, #0x554]        head index
24efa68 ldr w8,  [x19, #0x560]       entries per chunk
24efa6c ldr x9,  [x19, #0x518]       chunk pointer table
24efa74 ldr x24, [x19, #0x478]       "now"
24efa30 mov w28, #0x948              entry stride
24efa90 ldr w27, [x8, #0x8]          next index
24efa94 ldr x25, [x8, #0x228]        last-received stamp
24efa98 add x22, x8, #0x18           the entry's key
24efa84 mov w2,  #0x80               key length (memcmp against kInvalid)
24efaa4 cbz x0,  <previous>          key == kInvalid
24efae4 sub x25, x24, x25            age = now - stamp, unsigned, no clamp
24efa14 adrp x23, 0x381d000 ; 24efa18 ldr x23, [x23, #0x538]   the GOT word at ELF 0x371d538 holds &SAddress::kInvalid
```

## What it does

Why: the lobby-to-match switch can destroy the new game-server peer as "communication timeout" 0.25 s after connect. A stamp above "now" makes
`age = now - stamp` wrap to about 2^64 us and every timeout branch fires.

The handler: if a combat map or the combat lobby is loaded (below), one read of `bd[+0x470, +0x564)` (a failed read leaves it zero, so `per == 0` and there is
nothing to do), one read of `kInvalid` (0x80 bytes; unreadable: nothing is clamped), then the stock walk: from the head index, each entry at
`chunks[idx / per] + (idx % per) * 0x948`; one read of the entry span `+0x8..+0x230` (unreadable: the walk ends); an entry whose key (0x80 bytes at +0x18) differs
from `kInvalid` and whose stamp (+0x228) is above `now` gets its stamp set to `now` (an 8-byte store at an address just read; `now` is never written; stamps at or
below `now`, a real silence included, are untouched); `idx = next` until the end marker 0xffffffff or 4096 entries (one Warning, once). Then the game's function runs,
always, with the same argument. The clamp happens before the call and logs before it too, so no game frame is live above a log.

Install: the scope must be live first (`scope::Live()`: the `scope` row of the install table registered the refresh and the tick is installed; else `scope_not_live` and nothing is hooked), the patched build ID, the 26 code words, the pointer word at `base + 0x371d538` (null or unreadable: refused, `invalid_unreadable`),
then the thunk on the slot, which must hold exactly `base + 0x24ef9b0`.

## Arena cost and arena behaviour (arena and combat share the APK)

- **Behaviour.** This guard writes the game's memory (a peer's stamp); the plugin declared it altering gameplay and upstream it was test only. So it is scoped: it acts
  only while a combat map or the combat lobby is loaded (`combat_scope.h`, `SeenCombatOrLobby()`; an Echo Arena level that is loaded wins over any combat level still loaded
  during a transition; a true answer older than `kStaleAfterMs`, a tick that stopped, is not believed; `mpl_lobby_b`, `_b2`, `_b2_lowspec`, `_b_arena` and `_b_social`, the lobbies arena players use, never enable it). In an arena match the handler calls the
  game's function with the same argument after a load of the scope word and a test: no read, no write, no counter, no line, no level scan. The combat body is another function (`ActInScope`, not inlined) that begins with `scope::LookIsFresh()`. `peerclamp_test` runs a future stamp in all four Echo Arena levels, in the arena lobbies, with a level nobody
  names, with nothing loaded, before the scope's first look, and with an arena level loaded beside a combat map or the combat lobby: the stamp is left alone, by count
  (`SetReadCounterForTest`, `level::ScanCalls()`) nothing is read and no level is scanned, the game's function runs once; with a combat answer from a tick that stopped it is the same. Even in scope the clamp changes nothing for a healthy session: it acts only when `stamp > now`, which a healthy peer never has.
- **Known limits.** The scope's look lags a level change by up to a quarter of a second. A lobby-to-match switch from a lobby that is not the combat lobby is not covered: whether the
  race happens there is not established. `combat_peerclamp_clamped` and `combat_peerclamp_stats` on a headset would show it.
- **Cost.** `CleanupPeers` runs once per `CBroadcaster::Receive`; how often per frame is not known (the module counts: `combat_peerclamp_stats`). In arena each call pays the thunk's counter
  and one relaxed load of the scope word and a test; the scope's own refresh (`combat_scope.h`) adds one clock read per frame and, every quarter second, one `level::Scan` (two table reads and, per gamespace, three more `process_vm_readv` reads), once for every module that reads it.
  In scope a call also reads the clock (`LookIsFresh`) and pays `2 + (one per peer)` `process_vm_readv` calls and the counters. Host numbers (x86-64, -O2, 2,000,000 calls (100,000 in a combat map) through the installed thunk on a libr15 image built in memory, a stand-in `CleanupPeers`, a broadcaster with two healthy peers, three runs, a scratch benchmark that is not committed, on a machine with nothing else running): an unhooked slot 1.3 ns per call; hooked, in an Echo Arena level, 2.1 to 2.2 ns per call (0.8 to 0.9 ns added); hooked, in a combat map (four `process_vm_readv` calls, no clamp), 2.69 to 2.78 us per call. The arm64 handler (NDK r26d clang, `llvm-objdump -d`) is `adrp; mov; ldr w8; tbnz; and; cbz` and then a `br` to the game's function (arena) or a `b` to `ActInScope`: no frame, no call, no canary. In scope up to three calls per frame stay under ADR 0038's 10 us a hook, four would not; the call rate is what the stats line measures.
  Not measured on a headset.

## Gate: no file hash

The gates are the patched build ID the install checks, the 26 code words (read before the slot is touched) and the slot's own expected word. A word that differs is refused
with one Error `combat_peerclamp_install` line (`reason` `pinned_word_mismatch`, `pin` (the word's name), `vaddr`, `found`, `expected`; `found` is `unreadable` for a page that cannot be read). The pins
are `kWords` in `peerclamp_targets.h` (`pinned::Word`; the offsets the walk reads are tied to them by `static_assert`; checked by `pinned::FirstFailure`); `got_pinned_test` reads every one in the store libr15. `peerclamp_test` flips each in turn, reads one from an unreadable page, and pins the
table by a fingerprint.

## Lines (sentinel log)

- `combat_peerclamp_install`: `result` `installed|refused|off`, `reason`, `hook`, `scope` `combat_map_or_lobby`, `consequence`. Info for `installed` and `off` (reason `off_config`), Error
  for a refusal (`module_not_loaded`, `no_build_id`, `build_id_mismatch`, `pinned_word_mismatch`, `invalid_unreadable`, `scope_not_live`, `slot_refused`).
- `combat_peerclamp_clamped`: Info, the first 16 clamps (`n`, `idx`, `stamp`, `now`, `diff`, `sent`); then `combat_peerclamp_clamped_total` (`clamps`) at every 100th clamp, at most 8.
- `combat_peerclamp_walk_capped`: Warning, once (`entries` 4096).
- `combat_peerclamp_log_cap`: Warning, once per kind (`clamped`, `clamped_total`), written by the first line over its cap, saying where the count goes on (the stats line, never capped).
- `combat_peerclamp_stats`: `calls`, `in_scope`, `walked`, `clamped`, `capped`, `final`; Debug every 4096 in-scope calls, Info once at the end of the process when the guard is
  installed.

## Prediction (written before any headset run)

On a combat APK: `combat_peerclamp_install` `installed`. Across lobby-to-match transitions in combat no new game-server peer is destroyed with "communication timeout" 0.25 s after connect.
In most sessions zero `combat_peerclamp_clamped` lines (the race is rare); when it happens one line with a small or huge `diff` and the peer survives. In an Echo Arena match on the same APK
`in_scope` stays 0 and `clamped` stays 0.

## Falsifiers

- `combat_peerclamp_install` `refused` with `pinned_word_mismatch` on a build the sentinel accepted by build ID: the code at that address is not what the handler was derived from.
- The 0.25 s "communication timeout" disconnect still occurs and no clamp line was logged with `in_scope` above 0: the stamp is not above `now` (`now` is stale or the age is computed elsewhere), or the
  walk missed the entry (layout wrong). With `in_scope` 0: the scope did not enable it (the switch came from a lobby that is not the combat lobby, or the combat tick stopped: `combat_scope_stale` lines).
- Clamp lines in every session at steady state: `now` lags the stamps normally, and the clamp would mask a real silence (peers that left never time out).
- `invalid_unreadable`: the GOT word at 0x371d538 is not relocated when the step runs.
- `combat_peerclamp_walk_capped`: the peer list loops or is longer than 4096.
- `clamped` above 0 in an Echo Arena match: the scope is wrong.

## Not established

- On a headset: that the install succeeds, the call rate and cost of `CleanupPeers`, and that the race is gone.
- That `bd+0x470` is the "sent" counter (a log field only), that `kInvalid`'s bytes are stable for the process (re-read each call), and the single-thread claim (the reference build's).
