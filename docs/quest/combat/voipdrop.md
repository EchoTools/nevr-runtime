# Module `voipdrop` (row R30)

Files: `src/quest/combat/voipdrop.{h,cpp}`, `src/quest/combat/voipdrop_targets.h` (the pins), `src/quest/tests/voipdrop_test.cpp`,
`src/quest/combat/mutants.d/voipdrop.sh`. The module is a normal-phase row of the combat install table (`src/quest/combat/combat_install.h`): it runs in
the combat APKs and in no other. Switch in `config.yaml` (docs/adr/0046 rule 4): `combat.voipdrop` (default true). A
`voipdrop.off` file is not read; one found in the data directory (`legacy_switch.h`) is a Warning (`combat_voipdrop_legacy_switch`) that names the key.

Origin: Ducky's echoquest-combat reference build, afk_voipdrop.cpp (the rule at :1, the constants at :42-43 and :51, the pinned words at :55-64, the
probe at :82-94, the hook at :96-112 and the switch at :114-130; docs/audits/2026-11-05-q15t-origin-citations.md rows 133-136 and 279-280), reconstructed in the
combat plugin imported in #1591. The plugin's doubt rule (every unreadable field runs the game's function) and its code-word pins are
additions to the source's.

## Row covered

R30: `NRadEngine::NRadGame::CR15NetVoipReceiverCS::ReceiveVoip(SCompactPoolHandle, const SR15NetUserVoipEvent&, const void*, unsigned)`, JUMP_SLOT ELF
0x36c5d80, real function ELF 0x21c3238 (disassembly 0x22c3238, 1192 bytes). `readelf -rW` of the store libr15 (sha256 `8dd9a961...`, build ID `b243509c...`)
lists `00000000036c5d80  R_AARCH64_JUMP_SLOT  00000000021c3238 _ZN10NRadEngine8NRadGame21CR15NetVoipReceiverCS11ReceiveVoip...`. The words the guard was derived
from (ELF address, word, instruction):

```
21c3238 d10203ff sub sp, sp, #0x80          ReceiveVoip entry
21c3698 9105a2a0 add x0, x21, #0x168        the voice hash table inside the receiver
21c36b4 97b5dab7 bl  FindOrCreate
2316... AllocPage 0x23164d0..0x231650c: bl CMemBlock::Allocator (x21), bl CurrentPtr (x20), bl SetCurrent(x21), bl Current, ldr x8,[x0],
        ldr w1,[x19,#0x64], ldr x8,[x8,#0x10], blr x8; the result is not tested before use
23164d8 91006016 add x22, x0, #0x18         the table's allocator block (table+0x18)
2316504 b9406661 ldr w1, [x19, #0x64]       page bytes (table+0x64)
2316508 f9400908 ldr x8, [x8, #0x10]        allocator vtable +0x10 = the page request
23165a8 b9005269 str w9, [x19, #0x50]       the free head (table+0x50)
```

DestroyContext frees through the same vtable at +0x38 (`ldr x8,[x8,#0x38]`, 0x124943c). The four engine functions the probe calls are libr15 exports found with `dlsym`:
`CMemBlock::Allocator() const`, `CMemoryContext::CurrentPtr()`, `CMemoryContext::SetCurrent(CAllocator*)` and `CMemoryContext::Current()`
(`voipdrop_targets.h` names them; their addresses are not pinned, as in the plugin).

## What it does

Crash (the reference build's voipdrop:1-22, tester dump c63511b8): `CHashTable<uint,ull>::AllocPage` asks the table's allocator for one page and never checks the result;
a NULL page is written from its end backwards (SIGSEGV at 0x7ffe0). The table frees every page each frame (UpdateDefragment), so every packet after that needs a fresh
page: a heap with no free 512 KiB block crashes on the next voice packet.

The handler (a `CallbackThunk` on the slot, installed by the hook manager): read the free head (`int32 [cs+0x1b8]`). If it is not -1, call the game's function. If it is -1, read the
page size (`u32 [cs+0x1cc]`) and probe: save `CurrentPtr()`, `SetCurrent(Allocator(cs+0x180))`, `Current()`, call vtable `+0x10(a, bytes)`, give a non-null block back through
vtable `+0x38`, restore the saved context. A failed request (or no allocator at all) drops the packet: the game's function is not called, a counter is kept and `combat_voipdrop_dropped`
is written (the first 16; then `combat_voipdrop_dropped_total` at every 50th drop, at most 16). Doubt (self null, the page size unreadable, the vtable unreadable or missing an
entry) calls the game's function with the same arguments. The probe is not a reservation: it narrows the crash to a race, it does not close it; the table's own re-insert in UpdateDefragment
is not covered.

## Arena cost and arena behaviour (arena and combat share the APK)

- **Behaviour.** The guard acts in every level, on purpose: it is a crash guard of voice code that arena and combat share, so a scope by level (`combat_scope.h`) would leave the crash in place (and
  could lag a level change); it asks nothing of the scope and is not a reader of the scope row. It changes what the game does only where the game would otherwise fault: that packet is dropped. Every other packet reaches the stock function with the same
  arguments (`voipdrop_test`: the arguments, the free-page case, head 0, every doubt). An arena match with free memory sees no change; on a plain release the module installs nothing.
- **Cost.** `ReceiveVoip` runs once per remote voice packet; the rate per frame is not known, and `combat_voipdrop_stats` counts it (`packets`, `probed`, `dropped`; written at the end of the
  process and every 16384 packets). In the common case each packet pays the thunk's counter, two relaxed atomic increments and one plain 4-byte load (the free head, `[self+0x1b8]`): `ReceiveVoip` can return before touching `self`
  (`cbz x8` on a global at 0x21c3260 returns first when the global is null) and otherwise loads `[self+0xb8]` at once (0x21c3264). So on every path that reaches the table the stock function dereferences `self`, but on the early-return path it does not, and this load is one the stock function would not make. That `self` is a valid receiver component system wherever the game calls `ReceiveVoip` is an inference (it is the `this` the game passes), not shown here; a non-null unreadable `self` would fault in the guard and not in the stock function on that path. A null `self` goes to the game's function. Only a packet
  whose table needs a page pays the second read, three game calls and the context restore. Host numbers for the common path (x86-64, -O2, 5,000,000 calls through the installed thunk, three repeats, a temporary benchmark that is not committed): before the change to a plain load (one `process_vm_readv`): 418 to 429 ns added per packet over a 1.2 to 1.4 ns direct call (the same machine, one session; 840 to 895 ns earlier on a busier one); after: 2.9 to 3.2 ns added (re-run on the merged tip: 4.2 to 4.5 ns hooked over a 1.3 to 1.4 ns direct call, the same 2.9 to 3.1 ns added). At that figure the guard stays inside ADR 0038's 10 us per hook up to about 3,000 packets per frame, and the rate is what `combat_voipdrop_stats` measures. Not measured on a headset.
- **The probe, and whether it is a steady state.** With the free head at -1 every packet, from an existing speaker too, pays one allocate and one free on the table's allocator before the game's function runs, and a packet is dropped when the allocator says no even if `FindOrCreate` would have found the speaker's entry and allocated nothing (an allocator already out of memory). `AllocPage` (0x23164d8) links `[table+0x60]` entries of 0x20 bytes into the free list and sets the head to the first; the head is -1 exactly when every entry of every allocated page is in use. So -1 persists while the live voice entries fill their pages exactly; the entries per page is a run-time value that was not read here, so whether a 4v4 match reaches it is not established. `probed` against `packets` in `combat_voipdrop_stats` is the falsifier: a ratio near 1 over a match is a steady state.

## Gate: no file hash

The gates are the patched build ID the install checks, the 10 code words above (read before the slot is touched), the four exports, and the slot's own expected word: it must hold exactly
`base + 0x21c3238`, so a word something else replaced is refused, not chained over. A word that differs is refused with one Error `combat_voipdrop_install` line (`reason`
`pinned_word_mismatch`, `pin` (the word's name), `vaddr`, `found`, `expected`; `found` is `unreadable` for a page that cannot be read). The pins are `kWords` in `voipdrop_targets.h` (`pinned::Word`; the table and page offsets are tied to them by `static_assert`; checked by `pinned::FirstFailure`); `got_pinned_test` reads every
one in the store libr15. `voipdrop_test` flips each in turn, reads one from an unreadable page, and pins the table by a fingerprint.

## Lines (sentinel log)

- `combat_voipdrop_install`: `result` `installed|refused|off`, `reason`, `hook` (the manager's status), `consequence`. Info for `installed` and `off` (reason `off_config`), Error for a refusal
  (`module_not_loaded`, `no_build_id`, `build_id_mismatch`, `pinned_word_mismatch`, `export_missing`, `slot_refused`).
- `combat_voipdrop_dropped`: Info, the first 16 drops (`n`, `page_bytes`). `combat_voipdrop_dropped_total`: Info at every 50th drop after them, at most 16 (`n`).
- `combat_voipdrop_log_cap`: Warning, once per kind (`dropped`, `dropped_total`), written by the first line over its cap, saying where the count goes on (the stats line, never capped).
- `combat_voipdrop_stats`: `packets`, `probed`, `dropped`, `final`; Debug every 16384 packets, Info once at the end of the process when the guard is installed.
- `combat_voipdrop_legacy_switch`: Warning, when the plugin's `voipdrop.off` is found.

## Prediction (written before any headset run)

In normal play the module is invisible: `combat_voipdrop_install` `installed` at start, `combat_voipdrop_stats` with `dropped` 0, and no `combat_voipdrop_dropped` line in a session with free heap.
Under heap exhaustion (the crash's precondition) the log shows `combat_voipdrop_dropped` with `page_bytes` 524288 instead of the SIGSEGV in `AllocPage+0xc0` (`FindOrCreate+0xa4` from
`ReceiveVoip+0x480`); voice from that peer is dropped for those packets and resumes when memory returns.

## Falsifiers

- `combat_voipdrop_install` `refused` with `pinned_word_mismatch` on a build the sentinel accepted by build ID: the code at that address is not what the handler was derived from.
- A `combat_voipdrop_dropped` line in a normal session on a roomy heap: the layout offsets (+0x1b8, +0x1cc, +0x180) or the vtable slots are wrong, the probe fails for a reason other than
  memory, and voice is being dropped without cause (remote players inaudible).
- `page_bytes` other than 524288 in a drop line: the page-size field is not what AllocPage uses.
- The same SIGSEGV with no preceding drop line: the probe succeeded and the real request failed (the declared race), or the head test never saw -1.
- `combat_voipdrop_install` `refused` with `export_missing`: one of the four memory exports is not in this library.

## Not established

- On a headset: that the install succeeds on a combat APK, the packet rate and the cost per packet, and that the crash is gone under memory pressure.
- The allocator's vtable layout beyond the two slots read in the disassembly (+0x10 at AllocPage 0x2316508, +0x38 at DestroyContext 0x124943c, the second being the reference build's
  statement that it is the free) and the exact signatures of the four exports (taken from the reference build).
