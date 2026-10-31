# impactsnd: bullet impact sounds (rows R12 to R16, and R17: the Shutdown lock)

Port of the source build's `the source build's impactsnd` (v1 to v6) to a plugin module. Files: `src/impactsnd.{h,cpp}`,
`src/quest/tests/q15t/impactsnd_test.cpp`, `src/quest/tests/q15t/mod/impactsnd.sh`, `src/quest/tests/q15t/mutants.d/impactsnd.sh`. Pinned build: libr15 build id
`b243509c08ce677aeb95fa348016949b3fc45230` (the disassembly). ELF vaddr = disassembly VA - 0x100000. Source citations are
`the source build's impactsnd:<line>`; disassembly citations are quoted from read-only queries (scratch SQL, a source-build note not in this tree,
run with the contract's recipe).

## The problem

The Quest logs `Cannot find default impact sound for bullet type 0x<gun>` on every bullet hit and plays no impact sound
(`:1-2`). The engine's impact table `CR15NetBulletCS::kSoundSettings` holds 16-byte rows `{u64 key = sym(subkey) ^ sym(gun),
u32 id, pad}`, looked up by binary search on the key. Two things are wrong on the Quest:

1. The loader builds only 5 of the 15 rows (only the first member of each gun, `:19-23`). That is the loader's own scratch
   living on top of a path string: the fix is a code change (R16, not built here).
2. Every row's id is the stock `1` instead of the Wwise event id: `CSoundSymbol::CalculateSymbolValue` returns 1 for every name
   on the Quest (`:38-43`, RE-28). The fix is a data write to each row (built here).

The hit code (`HandleBulletCollision`, disassembly `0x2220550`, size 2264) does two lookups. Quoted from the disassembly raw
decompilation (`functions.va = 0x2220550`, scratch `hbc_decomp.txt` lines 123-195): the first looks up
`gunSym ^ <subkey read from the target's damageable record>` (lines 123-157, plays `*kSoundSettings + idx*0x10 + 8` through
`PlayCollisionSound`); the second looks up `D ^ gunSym`, D = `DAT_038af8d0` (lines 162-189), and on a miss calls
`NWriteLog::WriteLog(8,0,"Cannot find default impact sound for bullet type %s",...)` (line 195). The sort-pending bit is
read as `*(byte *)(kSoundSettings + 7) & 1` = map+0x38 bit 0, the Sort call follows, and the bit is cleared (lines 133-137,
166-170). The row layout (stride 0x10, key at +0, id at +8 passed to PlayCollisionSound) is the decompiler's own reading
(`*plVar21 + uVar1 * 0x10`, `+ 8`), the same layout the source build and this module use. Whether the first lookup's subkey is
`player` or `barrier` is the source's naming (`:103-119`), not something I read in the engine: unverified.

## Rows

| Row | What | ELF vaddr | disassembly VA | Built |
|---|---|---|---|---|
| R12 | `kSoundSettings` (.bss, `_ZN10NRadEngine8NRadGame15CR15NetBulletCS14kSoundSettingsE`, OBJECT 8) and its GLOB_DAT slot | `0x37af6a0`; slot `0x370fa70` | `0x38af6a0`; `0x380fa70` | yes (read; GLOB_DAT word cross-checked) |
| R13 | `D`, the "default" symbol word the hit code reads (.bss) | `0x37af8d0` | `0x38af8d0` | yes (write only when it reads 0) |
| R14 | `LoadSoundSettings` (`..CR15NetBulletCS17LoadSoundSettingsEv`, FUNC 808) and its JUMP_SLOT | fn `0x2121d04`; slot `0x36fa3c0` | fn `0x2221d04`; slot `0x37fa3c0` | yes (GOT wrapper, after-load pass) |
| R15 | the four hit-code words | `0x21208b4 / b8 / bc / c8` | `0x22208b4 / b8 / bc / c8` | checked, never changed |
| R16 | the five `.text` words of `kPatch` in `LoadSoundSettings` | `0x2121da8 / e2c / e30 / ea0 / eb0` | `0x2221da8 / e2c / e30 / ea0 / eb0` | NOT built: needs a word patch |
| R17 (new, from the bug-hunt review) | `CR15NetBulletCS::Shutdown` (FUNC 348) and its vtable word | fn `0x21226f0`; slot `0x36933e8` | fn `0x22226f0`; slot `0x37933e8` | yes (vtable-word wrapper holding the table lock) |

### Segment check (the .bss writes need no mprotect)

`readelf` on the store libr15 (`lib/arm64-v8a/libr15.so`, build id read
back `b243509c08ce677aeb95fa348016949b3fc45230`):

```
  LOAD           0x35f8eb0 0x00000000035f9eb0 0x00000000035f9eb0 0x1672c8 0x1d8688 RW  0x1000
  GNU_RELRO      0x35f8eb0 0x00000000035f9eb0 0x00000000035f9eb0 0x132150 0x132150 R   0x1
  [20] .got              PROGBITS        00000000036bf480 36be480 06cb78 08  WA  0   0  8
  [22] .bss              NOBITS          0000000003761180 3760178 0713b8 00  WA  0   0 64
  19337: 00000000037af6a0     8 OBJECT  GLOBAL DEFAULT   22 _ZN10NRadEngine8NRadGame15CR15NetBulletCS14kSoundSettingsE
  58011: 0000000002121d04   808 FUNC    GLOBAL DEFAULT   10 _ZN10NRadEngine8NRadGame15CR15NetBulletCS17LoadSoundSettingsEv
000000000370fa70  00004b8900000401 R_AARCH64_GLOB_DAT     00000000037af6a0 _ZN10NRadEngine8NRadGame15CR15NetBulletCS14kSoundSettingsE + 0
00000000036fa3c0  0000e29b00000402 R_AARCH64_JUMP_SLOT    0000000002121d04 _ZN10NRadEngine8NRadGame15CR15NetBulletCS17LoadSoundSettingsEv + 0
```

The RW LOAD ends at `0x35f9eb0 + 0x1d8688 = 0x37d2538`; RELRO ends at `0x35f9eb0 + 0x132150 = 0x372c000`. D (`0x37af8d0`)
and `kSoundSettings` (`0x37af6a0`) are in `.bss` (`0x3761180` to `0x37d2538`), above the RELRO end: writable at run time, no
mprotect (the module writes D with one aligned store after checking the page is `rw-` and not executable in
`/proc/self/maps`). The JUMP_SLOT `0x36fa3c0` and the GLOB_DAT `0x370fa70` are in `.got`, inside RELRO (read-only after
relocation): the slot swap needs the one-page mprotect that `got_swap.cpp` does and undoes (data page only, never executable).

### The Shutdown slot (R17), proven

From finding 7 of a source-build report (not in this tree): the disassembly slot `0x37933e8` -> `0x22226f0`. Checked against the store libr15:

```
$ readelf -rW libr15.so | grep -E "^00000000036933e8|^00000000036933e0|^00000000036933f0"
00000000036933f0  0000c24f00000101 R_AARCH64_ABS64  00000000013e744c _ZN10NRadEngine16CComponentSystem15OnArchiveUnloadERNS_16CArchiveResourceE + 0
00000000036933e0  0001112500000101 R_AARCH64_ABS64  000000000135ec84 _ZN10NRadEngine16CComponentSystem16RemoveReferencesEv + 0
00000000036933e8  00005ec900000101 R_AARCH64_ABS64  00000000021226f0 _ZN10NRadEngine8NRadGame15CR15NetBulletCS8ShutdownEv + 0
$ readelf -sW libr15.so | grep " 00000000021226f0 "
 24265: 00000000021226f0   348 FUNC    GLOBAL DEFAULT   10 _ZN10NRadEngine8NRadGame15CR15NetBulletCS8ShutdownEv
  [18] .data.rel.ro      PROGBITS        00000000035fac50 35f9c50 0c45c0 00  WA  0   0 16
  GNU_RELRO      0x35f8eb0 0x00000000035f9eb0 0x00000000035f9eb0 0x132150 0x132150 R   0x1
```

The slot is a vtable word: an `R_AARCH64_ABS64` relocation to `Shutdown` between two `CComponentSystem` virtuals, so after
relocation it holds `base + 0x21226f0`. It lies in `.data.rel.ro` (`0x35fac50` to `0x36bf210`), inside RELRO (ends `0x372c000`):
read-only at run time, swapped with the same one-page mprotect and restore as a GOT slot (`got_swap.cpp`, which also refuses a
page that is executable). disassembly: `22226f0 | Shutdown | 348`; xrefs to the function: `33c37c4 data`, `35df8b8 data`, `37933e8 data`
and no call or branch xref, so the vtable word is the only dispatch path the disassembly sees (the call graph is measured, not
exhaustive; the other two `data` xrefs parallel LoadSoundSettings' `33c37a4` / `35df808` at the same distance pattern, look like
table entries, and I did not read them: unverified). the disassembly xrefs to the slot word itself: 0 rows (a relocated word has none);
the `symbols` row is `DAT_37933e8`. Instruction words read (`instructions`):

```
22226f0 str x24,[sp,#-0x40]! ... 2222700 mov x19, x0 (this, read first; no other argument is read)
2222784 bl 0x00fd3590 ; 222278c adrp x24,0x380f000 ; 2222790 ldr x24,[x24,#0xa70] (the GLOB_DAT slot of kSoundSettings) ; 2222798 ldr x22,[x24]
22227bc bl 0x0100e200 ; 22227d8 bl 0x0100e200 (x23 = map+0x40, x22 = map; the callee is unread: unverified that it releases blocks)
22227dc ldr x8,[x21] ; 22227e8 ldr x8,[x8,#0x38] ; 22227ec blr x8 (the allocator's free, x1 = the map x22)
22227f8 str xzr,[x24]                      <- the global is cleared AFTER the free
2222760..2222774: subs x9,x9,#1 ; str x9,[x8] ; b.ne 0x2222834   (only when the last system leaves)
2222834 mov x0,x19 ... 2222848 b 0x010190d0   (a tail call: x0 is the method's return)
```

So between the `blr` at `0x22227ec` (the free of the map, x1 = x22) and the store at `0x22227f8` the global still points at
the freed map, and `0x22227bc` / `0x22227d8` run on the map before it (callee unread); a row store at any of those times can land in
freed memory. I read the instructions, not what `0x0100e200` and `0x00fd3590` do: the free is the vtable call at `0x22227ec`
(`[x21]->0x38`) by the report's account and the instruction shape, unverified beyond that.

## Disassembly reads (quoted)

`instructions` rows, bytes shown little-endian as stored:

```
R15  22208b4 | ldr  | x1, [x22, #0x30]   | c11a40f9   = 0xf9401ac1   (source kCodeCount)
     22208b8 | cbz  | x1, 0x02220960     | 410500b4   = 0xb4000541   (kCodeCbz)
     22208bc | adrp | x8, 0x38af000      | 68b400f0   = 0xf000b468   (kCodeAdrp)
     22208c8 | ldr  | x27, [x8, #0x8d0]  | 1b6944f9   = 0xf944691b   (kCodeLoad)
```

All four equal the source's constants (`:80`). `HandleBulletCollision` covers them (`2220550 | HandleBulletCollision | 2264`).

```
R14  functions: 2221d04 | LoadSoundSettings | 808
     callers (call_edges): 10459b0 | thunk_FUN_02221d04      (the PLT stub)
     callers of the stub:  222160c | Initialize | 680, xref `2221844 | call`
     stub: 10459b0 adrp x16, 0x37fa000 ; 10459b4 ldr x17, [x16, #0x3c0] ; 10459b8 add x16, x16, #0x3c0 ; 10459bc br x17
     xrefs to LoadSoundSettings: 10459bc call; 33c37a4 data; 35df808 data; 37fa3c0 data   (37fa3c0 = the JUMP_SLOT)
     Initialize: 2221838 adrp x8,0x380f000 ; 222183c ldr x8,[x8,#0xa70] ; 2221840 str x19,[x8] ; 2221844 bl 0x010459b0
     LoadSoundSettings entry: stp x28,x27,[sp,#-0x60]! ... 2221d1c sub sp,sp,#0x420 ; 2221d20 add x0,sp,#0x220   (x0 overwritten, never read)
     LoadSoundSettings exit: 2222008 mov w0, w19 ; ... 2222028 ret            (the only ret; returns w0 = w19)
```

One call site (`Initialize`, through the stub and the slot): the source's "1 caller". The two `data` xrefs `0x33c37a4`
and `0x35df808` are not GOT slots; I did not read what they are (probably unwind or constant tables): unverified, which is
one reason the tick stays as a safety net. The source calls the entry `Initialize 0x2121844`; that is the call site inside the
function (`Initialize` is `0x222160c` in the disassembly, ELF `0x212160c`), not its entry. The table pointer is stored
(`str x19,[x8]` at `0x2221840`, through the GLOB_DAT slot `0x380fa70`) BEFORE the loader is called, so a table exists, empty or
half-built, while the loader runs: the wrapper therefore acts only after the loader returns.

```
R13  xrefs to D (VA 0x38af8d0): 10712fc | write   ;   22208c8 | read
     _INIT_355  functions: 1070e14 | _INIT_355 | 5060 ; 1070e3c adrp x29, 0x38af000 ; 1070e4c add x29, x29, #0x75c
     10712e4 add x10, x29, #0x174 ; 10712fc str x8, [x10]     => x29 + 0x174 = 0x38af75c + 0x174 = 0x38af8d0
```

So the disassembly shows a store to D in the library's own static initialiser (`_INIT_355`), contrary to the source's v1 comment ("D
is .bss with no visible store, so it reads 0", `:6-8`). That comment was superseded by the source's own device read (v2,
`:19-20`: "the device read of v1 showed D already set"). The constant itself occurs in `.rodata` of the store libr15
(`grep` of the bytes `9e5b615f998ad32f`: 2 hits, file offsets 45744392 and 45744600); I did not trace which load feeds the
`str` at `0x10712fc`, so "that store writes CSymbol64(default)" is unverified (the device read is the evidence). The module's
D write is therefore a guard that is expected to be a no-op.

```
R12  xrefs to kSoundSettings (VA 0x38af6a0): reads at 22207dc 2220894 22208b0 2220940 2221e9c 2221fa8 2222798 2222fd8 2223094;
     writes at 2221840 (Initialize, before the load) and 22227f8; data 380fa70 (the GLOB_DAT slot)
```

```
R16  2221da8 | add | x22, x8, #0xc     | 16310091 = 0x91003116
     2221e2c | ldr | w10, [sp, #0x18]  | ea1b40b9 = 0xb9401bea
     2221e30 | ldr | x11, [sp, #0x10]  | eb0b40f9 = 0xf9400beb
     2221ea0 | str | w0, [sp, #0x18]   | e01b00b9 = 0xb9001be0
     2221eb0 | str | x28, [sp, #0x10]  | fc0b00f9 = 0xf9000bfc
```

All five equal the source's `old` words (`:91-95`).

## Mechanism per row

**R15 is a read-only guard, not a patch (confirmed from the source).** `CodeMatches()` (the source build's impactsnd:192-204) reads the
four words with `source-build safe_read`, compares each with `kCode*` and returns a bool; a mismatch logs `OFF ... nothing written` and
`source-build impactsnd_install` returns (`:478`). The only stores to the library in the whole file are the D store (`:485`, `.bss`),
the row id stores (`:311`, heap) and the five `kPatch` stores in `PatchLoader` (`:456`, R16). The module checks the same four
words in `InstallImpactSnd` (`src/quest/plugins/q15t/src/impactsnd.cpp:356-364`), refuses the whole module on a difference, and never writes them: R15
needs no word patch. (R1_functions.md classes it "needs-code-patch" because the site is code; that classification is about where
the words are, not about a change.)

**R12 / R13 (the D write).** `InstallImpactSnd` reads the GLOB_DAT word at `base + 0x370fa70` and requires it to hold
`base + 0x37af6a0` (the relocated address of `kSoundSettings`, a cross-check of R12 against the running process), reads D, and
stores `CSymbol64("default") = 0x2fd38a995f615b9e` only when D reads 0 and the build is not a nowrite build
(`src/quest/plugins/q15t/src/impactsnd.cpp:351-409`). Build option `-DQ15T_IMPACTSND_NOWRITE=1` is the source's `nowrite=1`: D and the rows are read and
logged, never written; the wrapper and the tick still run (they observe). The test seam `SetImpactSndNoWriteForTest` flips it.
The value is also checked in the test against the engine's own hash (`abi/symbol_hash.h`, `CSymbol64Hash("default")`).

**The ids rewrite (the tick, R12).** `ImpactSndTick` (a `TickFn`, register with `AddGameTick`; the main seat wires it) polls the
table at most 4 times a second, exactly the source's `Tick`/`IdsTick` (`:387-416`, `:322-385`): first the count line (the count
is logged `kSettleS` = 2 s after it is first non-zero, or after `kWaitS` = 600 s since install with what is there), then the
watch for the whole process. Per table instance: the same data pointer and count on two consecutive polls, then a re-read of
map, data, count and flags right before the rows. A table whose sort-pending bit (map+0x38 bit 0) reads 1 is written once per
table (the source's early=1, v5) and only when EVERY key is one of the 15 (a racing sort on a task thread could land a store in
a row whose key the sorted pass never fixes); a sorted table is written whenever a known row needs it (`NeedsWrite`: the row
holds the stock id 1 or another known row's id; an id that is none of ours is never touched). A pass takes the table lock (below), re-reads the global table pointer, the data pointer
and the count under it and writes only if all three are still what the pass validated. One aligned u32 store at `row+8`,
read back; keys, order and the pad are never touched (the sort moves whole 16-byte rows, so an id written before it travels with
its key). The 15 `kIds` rows and their FNV-1 32 ids are the source's (`:103-119`); `ImpactSndRows()` exposes them.

**R17 (the Shutdown wrapper and the table lock): built.** The source's `RowPass` stores to `row+8` without pinning the table
(bug-hunt finding 7), and `CR15NetBulletCS::Shutdown` frees it (above). Our writes come from two places, the game tick and the
LoadSoundSettings wrapper on the loader's thread; neither is proven to run on the thread that runs Shutdown. So: one
`pthread_mutex_t` (a POD, `PTHREAD_MUTEX_INITIALIZER`, no constructor at load). The Shutdown wrapper (vtable word `0x36933e8`,
expected value `base + 0x21226f0`, checked by `InstallImportHook`) takes it BLOCKING and holds it across the real Shutdown;
it forwards x0 (this) and returns the real x0. A row pass takes it with `trylock` only: if a Shutdown holds it the table is about
to disappear, waiting buys nothing, and the game-thread tick must never wait (the pass is skipped, counted, and the caller
retries on the next poll; a skipped unsorted pass is not marked done). Under the lock the pass re-reads
`kSoundSettings`, the data pointer and the count (`SafeRead`) and writes only if they equal what it validated; the whole
row loop, including the read-back, runs under the lock, and nothing is logged under it. The loop's stores therefore cannot run
while Shutdown runs, on whichever thread each is. A thread already inside the Shutdown wrapper does not take the lock a second time
(a thread-local depth counter), so a nested Shutdown cannot self-deadlock. If the slot cannot be proven (any
`InstallImportHook` status but `kOk`: a different word, an unmapped page, a failed mprotect) the module does NOT write rows at all:
`g_writers_ok` stays false, the tick and the loader wrapper only read and log (`impactsnd ids (row writes disabled, Shutdown not
wrapped: ...)`, once), and the install logs an ERROR. D is a `.bss` word no Shutdown frees and is unaffected. Which threads run
the loader wrapper, the tick and Shutdown is logged (see Behaviour).

**R14 (the GOT wrapper): built, decision.** Source: the loader "fills the table row by row on the load; its thread is not
proven" (`:387-388`), so the source polls (`kPollS` 0.25 s, 2 s settle, two stable polls, a re-read), and its v4/v5 comments
record that the first hits of each table post id 1 while the poll catches up. In the plugin the swap of one JUMP_SLOT is the
same data-page mechanism as the other modules, and it gives an exact moment: the wrapper calls the real loader, then (after it
returned, so the table is complete and no hit has used it) reads the table and runs the same `RowPass` once. Reasons it is worth
having: (1) the window in which a hit posts id 1 closes (tick-only: at least the 2 s settle plus two polls after the table
appears); (2) the loader's row count is read at that moment on the loader's own thread, so every launch's log says whether R16
is in effect (`rows=15 word_patch=in_effect`, or `rows=5 word_patch=absent`); (3) there is one caller and the loader runs once per
level load (low fan-in), so a log line there is not per-call chatter. Reasons it is not a replacement for the tick: it cannot
cover a table built through a path that does not use the slot (the two unread `data` xrefs above), a failed slot install (the
module then runs tick-only and logs an ERROR), or a store lost to a racing sort. The wrapper passes the loader's whole return
value through (`mov w0, w19` at `0x2222008`, and `Initialize` does not clobber x0 before its next call at `0x222184c`, so the
value is kept intact rather than assumed unused) and takes no argument (x0 is overwritten at `0x2221d20` before any read). It
shares state with the tick only through atomics and the table lock (`g_uns_done` marks the table the wrapper already wrote, so
the tick's once-per-table unsorted pass does not repeat it; a pass the lock or the re-check skipped does not mark it).

## Behaviour

- Install, once: four hit-code words checked, GLOB_DAT word checked, D read (and written only if 0), the Shutdown word swapped,
  the LoadSoundSettings slot swapped. One line each: `impactsnd on` (D was/now/action), `impactsnd shutdown wrapper`
  (status, `row_writes=enabled` or ERROR `DISABLED (read and log only)`), `impactsnd load wrapper` (status, mode), or
  `impactsnd OFF: nothing written` (ERROR, with reason, vaddr, read, pinned). Any refusal before the wrapper: nothing is written and the tick does nothing.
- Each `LoadSoundSettings` return: `impactsnd LoadSoundSettings returned` (table, rows, flags, `word_patch`), then
  `impactsnd ids` when rows were rewritten (`when=load/unsorted`, rows, known, rewrote, right, left, `found_ids`).
- Tick: `impactsnd table` once (rows, map set/null, after_s, default_symbol as read then, `word_patch`; WARNING for a count
  that is neither 5 nor 15), `impactsnd ids` for any pass that wrote (`tick/unsorted`, `tick/sorted`), `impactsnd no table now`
  once per disappearance of the table, `impactsnd table still unsorted` once per table after 600 s.
- Threads: `impactsnd thread` the first time the tick, the loader wrapper or the Shutdown wrapper runs on a thread (and when a role
  moves): `role`, `tid` (gettid), and the last tid seen in each role (`tick_tid`, `loader_tid`, `shutdown_tid`, 0 = not seen yet),
  so one headset run shows whether they share a thread. `impactsnd CR15NetBulletCS::Shutdown returned` (the global's value then).
  `impactsnd row pass skipped, nothing written` (why: lock busy, or the table changed or was freed before the write).
- Log budgets: non-writing lines 12 per process, writing passes 200 per process; the first line over a budget is one WARNING
  that names it, then silence (counters in `GetImpactSndStats()` keep counting); thread lines 24. Every line is structured through `Log`.

## R16, rebuilt (the loader in C++; no code page is written)

The word patch below is not used. `LoadSoundSettings` is rebuilt in `src/quest/plugins/q15t/src/impactsnd.cpp` and runs in
place of the stock one: the JUMP_SLOT (`0x36fa3c0`) points at the wrapper, which calls `RebuildUnderLock`, then the after-load
pass. The rebuilt body walks the same file (`/json/r14/config|mp_sound_settings.json`) with the same calls, in the same order,
and appends the same fields to the same table; the difference is the scratch. The stock loader's `{key, id}` scratch is at
`sp+0x10/0x18`, the bytes of the path string it passes to the next `CJson::Next`, so only the first member of each gun was
kept. The rebuilt loader keeps the path in its own 512-byte buffer and the key and id in locals, so every member is appended.

- Stock steps reproduced (disassembly `0x2221d04` decompile and disassembly): memset the 512-byte buffer and format
  `%s|%s`; construct the CJson (`0xfd7760`, 16 bytes of frame); `LoadFromProjectSourceDb` (`0x1000140`); for each
  `bullet_impact` node the gun name (`NameAt`, `0x1017d40`), its `bullet_impact|<gun>` members (`Next`, `0x1028490`, third
  argument the previous node), key = CSymbol64(member) ^ CSymbol64(gun); then the `bullet_whizzby` nodes into the table at
  `kSoundSettings+0x40` with key = CSymbol64(name); each append grows by `max(cap, grow)` through `CMemBlock::Resize`
  (`0x103e400`) and sets the sort-pending bit (`+0x38` / `+0x78`); destroy the CJson (`0xfdcae0`).
- Changed: the id is `WwiseEventId` (FNV-1 32 of the lowercase event name), not `CSoundSymbol::CalculateSymbolValue`, which
  returns `1` on the Quest for every name (RE-28, the module's `kStockId`). The CSymbol64 keys are the SDK's `CSymbol64Hash`,
  which the module's keys were already checked against.
- Calls: each target is the stock loader's direct-call stub (ELF = disassembly VA - 0x100000), the same stub the stock `bl` jumps
  through. Each stub's first word is pinned in `kRebuildCallees` and read from the disassembly (`adrp x16`), and the rebuild is enabled
  only when all seven match and the Shutdown wrapper is in (`impactsnd LoadSoundSettings rebuild`, `mode=`). The nowrite build keeps
  the stock loader.
- Signatures, from the disassembly (libr15): `CJson::Next(char const*, void*) const` (0x10a0c30), `CJson::NameAt(void*) const`
  (0x10a0cdc), `CJson::TString(void*, char*) const` with the fallback as a third register (the stock call passes `""`'s address
  as x2, `0x2221e94` / `0x2221fa0`; the thunk at 0x10a58f4 shows two declared parameters after `this`), `CMemBlock::Resize(ulonglong,
  uint)` (0x103e400; the stock also passes `1` in w2, passed here as `keep` and ignored by the declared signature).
- Not verified here: the CJson object size (16 bytes is inferred from the stock frame: the path buffer starts at sp+0x220 and
  the ctor runs after it is filled), the TString fallback semantics (read as the value when the key is missing), and whether
  `CMemBlock::Resize` ignores its third register. The headset run below settles these.
- Logs: `impactsnd LoadSoundSettings rebuilt` (`table`, `rows`, `json_result`, `loader=reconstructed`), then the usual
  `impactsnd LoadSoundSettings returned` with `rows=15 word_patch=in_effect` expected.

**Headset check (3 lines):** `adb logcat | grep -E "impactsnd (LoadSoundSettings rebuild|LoadSoundSettings rebuilt|LoadSoundSettings returned|ids)"`
at a combat level load. Expect `mode=reconstructed`, `rows=15`, `json_result=0`, `word_patch=in_effect`, and no
`Cannot find default impact sound for bullet type` on hits. Falsifier: `mode=stock` (a stub check failed: `missing_callee`
names it), or `rows` neither 15 nor 5.

## What is lost without R16 (superseded: the rebuilt loader above writes all 15 rows)

The loader then builds 5 rows, the `player` row of each gun (source `:19-23`, `:49`; the device read of v1/v2 saw count 5).
This module is correct in that state: the 5 rows are rewritten to the Wwise ids, D is guarded, the log says `rows=5
word_patch=absent`. What does not happen:

1. 10 of the 15 rows never exist: the `barrier` row and the `default` row of each of the 5 guns.
2. The hit code's second lookup (`D ^ gunSym`, disassembly `0x22208b0..0x2220960`) misses on every hit, so
   `Cannot find default impact sound for bullet type ...` keeps logging on every hit for every gun, and no default (`geo`)
   impact sound plays on world or non-player hits.
3. The first lookup for a `barrier` target misses (no row): no shield-impact sound.
4. Still working: the `player` row's `play_bullet_impact_player` (and `_quark` for smg) event on player hits, once rewritten. The
   D guard is moot without default rows.
5. The plugin cannot restore the missing rows: adding rows would mean growing the engine's `CTable` and re-sorting through the
   engine's allocator (not built, no evidence for it); re-keying the five `player` rows to `default` would break the player
   lookup and rewrites keys, which the source never does. So R16 is the only way to get rows 6 to 15.

## R16 word-patch spec for the main seat

Five 32-bit words, all in one 4 KiB page (disassembly page `0x2221000`, ELF `0x2121000`), no control flow changes. Stock words
read from the disassembly (above) and verified against the source; replacement words exactly as the source's `kPatch` (`:90-96`), and
disassembled here with the NDK's clang and `llvm-objdump` (a scratch listing, not in this tree) to confirm the
instruction each encodes:

| Disassembly VA | ELF vaddr | stock word | stock insn | replacement word | replacement insn |
|---|---|---|---|---|---|
| `0x2221da8` | `0x2121da8` | `0x91003116` | `add x22, x8, #0xc` (x8 = sp+0x10) | `0x910033f6` | `add x22, sp, #0xc` (the pad fill) |
| `0x2221e2c` | `0x2121e2c` | `0xb9401bea` | `ldr w10, [sp, #0x18]` | `0xb9400bea` | `ldr w10, [sp, #8]` (the id into the table) |
| `0x2221e30` | `0x2121e30` | `0xf9400beb` | `ldr x11, [sp, #0x10]` | `0xf94003eb` | `ldr x11, [sp]` (the key into the table) |
| `0x2221ea0` | `0x2121ea0` | `0xb9001be0` | `str w0, [sp, #0x18]` | `0xb9000be0` | `str w0, [sp, #8]` (the id) |
| `0x2221eb0` | `0x2121eb0` | `0xf9000bfc` | `str x28, [sp, #0x10]` | `0xf90003fc` | `str x28, [sp]` (the key) |

The effect (source `:19-24`): the loader's row scratch `{key, id, pad}` moves from `sp+0x10 / 0x18 / 0x1c`, where it sits
on top of the path string `bullet_impact|<gun>` handed to the next `CJson::Next`, to the frame's unused `sp+0x0..0xf`. The
claim that `sp+0x0..0xf` is unused is the source's (AUD-7 GP-1..GP-9); I read only that the five stock words match.
When to apply it: before the first `LoadSoundSettings` call. The table appears about 114 s after launch (source `:32-33`), and
the loader's only caller runs at the combat level load, so a patch applied at plugin init is early enough. The source also
checked, before writing, that the table was not built yet (map null or count 0) and refused otherwise (`:441-448`): the same
guard belongs on the word-patch row. After the patch, the module's own lines read `rows=15 word_patch=in_effect`; the source's
`STOP: patched launch but count=<n>, not 15` (`:411-413`) is this module's `impactsnd table` WARNING for a count that is neither
5 nor 15, since the plugin does not know whether the main seat's patch was applied. Back-out is a launch without the row.

## Prediction (written before any headset run)

P1. `impactsnd on ... was=<non-zero> now=<same> action=left alone (not 0)`. Basis: the disassembly shows `_INIT_355` storing D
(`0x10712fc`) and the source's device read found D set. Falsifier: `was=0 action=wrote` (D is not set at plugin init; then the
write is the fix and the disassembly reading is incomplete), or `was` non-zero and not `2fd38a995f615b9e` (the hit code would look
up another key; the module leaves it alone by rule, and the lookup stays wrong: the line is the evidence).

P2. `impactsnd load wrapper ... status=ok mode=after-load pass + tick`. Falsifier: any other status (for the Shutdown wrapper too: `impactsnd shutdown wrapper status=ok row_writes=enabled`; anything
else means rows are not written, and the ERROR line names why), or no
`impactsnd LoadSoundSettings returned` line at the combat level load (about 114 s after launch): the slot is not what
`Initialize` calls through, or the load runs before the plugin loads.

P3. At the combat level load, without R16: `impactsnd LoadSoundSettings returned ... rows=5 flags=00000001 word_patch=absent`,
then `impactsnd ids ... when=load/unsorted rows=5 known=5 rewrote=5 right=0 left=0 found_ids=00000001,00000001,...` (five
times). With R16: `rows=15 word_patch=in_effect`, `rewrote=15`. Falsifiers: `found_ids` not all `00000001` (the id-1 cause, RE-28, is
wrong or only partly right: the ids rewrite would be moot or incomplete); `rows` neither 5 nor 15; `flags` bit 0 clear at load
(sorted at load: `when=load/sorted`, harmless, but the "no hit before the rewrite" premise changes); `rewrote` below
`known` with `left>0` (stores did not take: a racing sort or a non-writable heap page).

P4. About 2 s after the table appears: `impactsnd table rows=5|15 word_patch=... default_symbol=2fd38a995f615b9e`. After the
first hit sorts the table, no further `impactsnd ids` line. Falsifier: a later `when=tick/sorted` line with `rewrote>0`:
something put the stock ids back or a store landed in a neighbour row (the module repairs it, and the line is the evidence).

P5. Audio and the engine log in a fight. Without R16: the player-hit impact event plays for assault, scout, rocket, rifle and
smg; `Cannot find default impact sound for bullet type` still appears on every hit; no shield or default impact sound. With R16:
that message stops for the five guns (chain and magnum have no row on the PC either and keep logging, source `:16-17`), and
player, shield and default impact events all play. Falsifiers: the message absent without R16 (rows exist: contradicts
`rows=5`), or still present with R16 and `rows=15` and `default_symbol=2fd38a995f615b9e` (the default lookup misses for another
reason: the rows' keys are not `sym(default) ^ sym(gun)` in the live table).

P6. Threads. `impactsnd thread` lines for the three roles. Prediction: tick = the game thread; the loader wrapper and the
Shutdown wrapper are the thread that runs level load and unload, which I expect to be the game thread too (the loader's only
caller is a level component system's `Initialize`), but that is NOT established: the source says "the THREAD of the load / unload
is not proven" (`:239-241`), and the lock exists because of that. Falsifier: any role on a different `tid` from `tick_tid` means
the unprotected source would have raced; with the lock, the line `impactsnd row pass skipped ... table lock busy` or `table
changed or freed before the write` appears only if a pass and a Shutdown actually overlapped.

## Not built, and why

- R16, the loader's five `.text` words: a code change; the contract forbids it in a module. Specified above. This module
  writes no code page at any point: its only stores are D (`.bss`, `rw-`, checked in `/proc/self/maps`), the row ids (heap, under
  the lock), and the two data-word swaps through `got_swap.cpp` (which refuses an executable page). The module has no
  `mprotect`, `PROT_EXEC`, `__builtin___clear_cache` or `memcpy` into the library.
- Adding the 10 missing rows without R16, or re-keying: see "What is lost without R16".
- The source's early log of "loader patched=" and `STOP` line: the module cannot know the main seat's patch state; replaced by
  the row-count inference in the `word_patch` field.
- The source's switches (`isnd.off`, `isnd.on`, `setprop ... isnd`, `patch=1`, `ids=1`, `early=1`): ids and early are always on
  (the source's default form, v6); `nowrite` is the compile-time option `Q15T_IMPACTSND_NOWRITE`; the on/off switch is the
  main seat's option `Q15T_IMPACTSND`.

## Wiring for the main seat (plugin.cpp, CMakeLists.txt; not edited here)

- Source file `src/quest/plugins/q15t/src/impactsnd.cpp`. Option `Q15T_IMPACTSND`, default ON (the source build's default since v6).
- In `NvrPluginInitEx`, before `InstallGameTick`: `Report("impactsnd (LoadSoundSettings)", InstallImpactSnd(ctx->base_addr));`
  and `want_tick = AddGameTick(&ImpactSndTick) || want_tick;`. `Report` prints the wrapper's status; the module's own lines carry
  the details. The module writes game data (D, table ids); capability flags are the main seat's call.
- Optional: `-DQ15T_IMPACTSND_NOWRITE=1` for a read-and-log-only build.
- Link `-pthread` (the module uses `pthread_mutex_t`; no namespace-scope constructor and no `__cxa_atexit` reference: checked on
  host and NDK aarch64 objects, `readelf` finds no `.init_array`/`.ctors`).
- `src/quest/tests/q15t/mod/impactsnd.sh` is picked up by `src/quest/tests/q15t/run_host_tests.sh` and `Q15T_MODULE=impactsnd`.

## Tests and mutants

`src/quest/tests/q15t/impactsnd_test.cpp` builds a fake image (an anonymous mapping that holds the pinned vaddrs: code words, the GLOB_DAT
word, `.bss` words, a read-only slot page, an executable trampoline at the loader's address) and a fake table on the host heap.
It checks, among others: the 15 keys against the engine's `CSymbol64Hash` and the 15 ids against an independent FNV-1 32 (the helper itself is
checked on the published vectors `""`, `"a"`, `"foobar"`); each of the four code words wrong, unreadable, the GLOB_DAT
mismatch, D unreadable / read-only / executable; the slot refused (tick-only mode); the wrapper on 15-row, 5-row, sorted,
foreign-key, empty and over-cap tables with the return value passed through and keys, order and pads untouched; every
poll-gating, stability, re-read (count, data, map, sort bit, table freed), once-per-table, fix-up (neighbour id, foreign id),
nowrite, log-budget and count-line rule. The race tests (two threads): a fake Shutdown (called through the swapped vtable word) that frees the table (poisons its rows,
clears the global) while a row pass is mid-flight (a stage-2 hook parks the pass inside its row loop until the other thread is in
the wrapper, then gives it 150 ms); the test fails if any row store follows the free or if Shutdown ran before the pass finished
(15 of 15 rows right at free). Without the lock (today's code has none) the free lands mid-pass and the test reports row stores
after the free. Also: Shutdown between a pass's validation and its lock (the global cleared; another table published; count, data
or the map object changed), the lock held by a Shutdown (the tick returns within 150 ms and writes nothing), a nested Shutdown, a
refused Shutdown slot (rows read and logged, never written), and the thread lines (same tid across roles, a different tid for a
Shutdown on another thread, a budget of 24). `src/quest/tests/q15t/mutants.d/impactsnd.sh` carries one mutant per rule.
