# Module `custctx` (rows R31, R32, R33)

Files: `src/custctx.{h,cpp}`, `src/quest/tests/q15t/custctx_test.cpp`, `src/quest/tests/q15t/mod/custctx.sh`, `src/quest/tests/q15t/mutants.d/custctx.sh`.

Option macro for the main seat to wire: `Q15T_CUSTCTX`, default **ON** (the source build's custctx:1 "DEFAULT ON", `Switch()`; installed from
the source build's tap:174). API: `InstallResult InstallCustCtx(std::uintptr_t base)` (all three hooks or none; one ERROR line and the first
failure's status on a refusal, an INFO `custctx on` line on success); seam `InstallCustCtxAt(base, addrs, fns)`; `GetCustCtxStats()`.
No tick. Runtime safety valve kept (the source's `custctx.off`): `<plugin data dir>/custctx.off` present at install: refused,
reason `off_file`, one INFO line. The `setprop` switch is dropped.

## Rows covered

| row | function | slot (ELF) | real fn (ELF = disassembly - 0x100000) |
|---|---|---|---|
| R31 | `CR15NetCustomizationRenderer::LoadMeshData` | 0x36cf6f0 | 0x1247d90 (disassembly 0x1347d90, size 2284) |
| R32 | `CR15NetCustomizationRenderer::DestroyContext` | 0x36c22f8 | 0x12492fc (0x13492fc, 72) |
| R33 | `CTableXT<SR15NetCustomDrawData>::Grow` | 0x36ed7f0 | 0x124893c (0x134893c, 408) |
| (helper) | `CMemBlock::Resize(u64,u32)` | called directly | 0x11dc30c (0x12dc30c, 216; R34 is its JUMP_SLOT 0x36f68e8, not hooked) |

Read this session: `R0_readelf_r.txt:114546` (`00000000036cf6f0 ... R_AARCH64_JUMP_SLOT 0000000001247d90 _ZN10NRadEngine8NRadGame28CR15NetCustomizationRenderer12LoadMeshData...`),
`:107763` (`00000000036c22f8 ... 00000000012492fc ...DestroyContext...`), `:129938` (`00000000036ed7f0 ... 000000000124893c _ZN10NRadEngine8CTableXTINS_8NRadGame21SR15NetCustomDrawDataEE4GrowEv`).
Disassembly `functions`: `1247d90 | LoadMeshData | 2284`, `12492fc | DestroyContext | 72`, `124893c | Grow | 408`, `11dc30c | Resize | 216`.
All 34 of the source's custctx code words (the source build's custctx:73-95) read in the disassembly equal the source (34/34 MATCH), including the PLT
stubs `ef0010 f0013ef0 / ef0014 f9437a11` (LoadMeshData, ldr x17,[x16,#0x6f0]) and `ed5820 b0013f70 / ed5824 f9417e11` (DestroyContext,
`ldr x17,[x16,#0x2f8]`), `1249644 97f29a73 bl <PLT LoadMeshData>` (CreateContext's single call), `124965c 97f23071 bl <PLT DestroyContext>`.
Grow read in the disassembly (the part the wrapper replicates):
```
1248948 09a042a9 ldp x9, x8, [x0, #0x28]     x9 = capacity, x8 = count
1248950 1f0109eb cmp x8, x9 ; 1248954 b.cc room
1248958 681240f9 ldr x8, [x19, #0x20]        step
1248964 1f0109eb cmp x8, x9 ; 1248968 3431889a csel x20, x9, x8, cc    add = (step < capacity) ? capacity : step
1248970 09e39252 mov w9, #0x9718 ; 1248974 017d099b mul x1, x8, x9      bytes = (add + capacity) * 0x9718
1248978 a2d6f397 bl <PLT Resize>             result never checked
1248984 691600f9 str x9, [x19, #0x28]        capacity += add
1248998 681a00f9 str x8, [x19, #0x30]        count + 1
12489c8 0a7af297 bl <PLT memset>             x0 = data + count*0x9718 - 0x9718: the crash site
```
RegisterRTLayout (render-target layout pool, v4): `18829c4 ldr x8,[x19,#0x2c0]` (pool pointer at 0x379b2c0), `18829c8 ldr w3,[x8,#0x84]`
(capacity), `18829cc ldr x9,[x8,#0x30]` (free count), `18829d0 sub x10,x3,x9`, `18829d4 cmp x10,x3`, then the log and `brk`.
`DestroyContext` is not hooked by a direct `bl` anywhere (all 10 callers go through the PLT stub; source `:4-9`; `R1_functions.md:45`
"9" callers counts the stub xrefs). Source constants: `the source build's custctx:63-65, 100-101`, hooks `:168-206, 208-230, 232-285`.

## What it does

- **R31 LoadMeshData wrapper.** Under one mutex: the context becomes *live* and gets a *load in flight* count (the only creation path
  is CreateContext's single call). v4: read the layout pool (`SafeRead` of `[base+0x379b2c0]`, `+0x84` capacity u32, `+0x30` free
  count u64); fewer than 64 free: the load is **refused** (returns 1, the stock failure value; CreateContext then destroys the empty
  context and returns NULL, checked by its caller) with an INFO line (first 16 refusals, then every 50th). Otherwise the real
  function runs. An unreadable/null pool pointer never refuses. Every 32nd load (at most 32 lines) logs the pool use.
- **R32 DestroyContext wrapper.** Null context: real (it returns at once). Otherwise, under the mutex, the real function runs only
  for a context that is live, with no load in flight and not already being destroyed; the entry is freed when the destroy returns.
  Every other destroy is **dropped** (returns 0) with one INFO line (`why` = unknown address / its LoadMeshData is running / already being
  destroyed; first 16 then a total every 50th, at most 16 totals). A dropped destroy changes nothing; the caller clears its own slot.
  More than 256 contexts tracked: the overflow context is untracked (stock behaviour for its load, its later destroy is dropped as
  unknown), one WARNING line, once. The mutex is never held across a stock call.
- **R33 Grow wrapper.** One SafeRead of the 0x38-byte table header. No room (count >= capacity): `add = (step < capacity) ? capacity :
  step` (the lines above), `CMemBlock::Resize(table, (capacity+add)*0x9718, 1)` called here and **checked** (re-read of `data`); on
  success capacity += add and the real Grow runs (it then only adds the element). Add 0, a NULL data after the resize, or room left but
  `data == NULL` (a torn-down table): the table is made consistent-empty (data/size/capacity/count 0; elements leaked, not destroyed) and
  the real Grow is run on a one-slot table of ours (`posix_memalign(64, 0x9718)`, or one of 4 spares preallocated at install, round
  robin) so LoadMeshData gets an element that is in no table; that player's cosmetics are not drawn. WARNING lines capped like above.
  An unreadable table: the real function.
- **Install.** The three slots each must hold exactly `base + their fn` (InstallImportHook with `expected_vaddr`), all or none (the
  earlier slots are put back with `SwapWord(slot, wrapper, real)` when a later one is refused); `CMemBlock::Resize` must be exported at
  exactly `base + 0x11dc30c` (dlsym), else `export_missing`; the 4 spares must allocate, else `spare_alloc_failed`.

Logging: INFO `custctx on` (the three slots, margin), ERROR `custctx refused (<hook>)` (slot, status, reason, pin / found / expected), INFO `custctx off`, INFO/WARNING
lines above. Per-call paths carry counters and a bounded line count.

## Gate: no MD5, no file hash

The only gates are: the plugin's build-id check in InitEx, the code-word pins below (`src/quest/plugins/q15t/src/word_pins.h`, `CheckWordPins`: every
word is read with SafeRead at `base + ELF vaddr` before any slot is touched) and the slot-target check (the slot must hold exactly
`base + the real function's vaddr`). **There is no MD5 gate and no whole-file hash of any kind**: a rebuilt, repacked or custom-server
build that still carries these words is not refused (the source build accepted three whole-file MD5s and refused every other
binary, which silently removed its crash guards). A pin that fails refuses the module (nothing swapped, reason `word_pin_mismatch`)
with one ERROR line naming it: `<module> refused: code word pin mismatch, ...  pin=0x<ELF vaddr> found=0x<word read, or unreadable>
expected=0x<word pinned>`. A slot-target refusal logs ERROR with `pin=<slot ELF vaddr> found=<word at the slot> expected=<base +
real vaddr>`. Pin addresses in this note are ELF vaddrs (disassembly VA = ELF + 0x100000). All 34 pins read in the disassembly
equal the table in `src/quest/plugins/q15t/src/custctx.h` (verified by a query that builds its VALUES from the header tables: `MATCH | 88` over the five
modules' tables, no NOT_SHOWN, no MISMATCH). Host test: every pin flipped in turn is refused and named, an unreadable text is refused and
named, the table's fingerprint is pinned, the installer refuses on a changed word (`src/quest/tests/q15t/pin_check.h`).

The 34 pins are the source's `kWords` (the source build's custctx:78-95, the same 34 words and the three JUMP_SLOT targets that the disassembly-verified
review found matching).

## Differences from the source build

- The three whole-file MD5s are deliberately not carried (see "Gate"; the source refused a fourth known binary and any custom-server build);
  the 34 code-word pins and the slot-target checks are kept; the file/property switches are replaced by the option and the `custctx.off` valve.
- Entry `Release` does not test the in-flight count (every load sets `live`, so a load in flight has live != 0: the extra test is
  unreachable), and the dead `!live` test stays only as the second line of defence for the null-context guard.
- The table header and pool words are read with SafeRead (an unreadable table goes to the real function); writes to the table are
  plain stores to the object the engine passed (already read successfully).

## Prediction (written before any headset run)

At start `custctx on ... status=ok`. In a lobby with cosmetics changing: no crash `memset(NULL)` in `Grow <- LoadMeshData` and no brk in
`CGRenderTargetState::Initialize` ("Exceeded the max number of registered render target layouts (512)"). Every 32 loads a line
`custctx cosmetic loads loads=32 layouts_used=<n> layouts_capacity=512` (n below 448). In a healthy lobby **no** `custctx dropped destroy`
line, no `custctx no memory ...` line and no `cosmetic load refused`. Under memory or layout-pool exhaustion the matching line appears
instead of the crash and that player's cosmetics are missing until the next load.

## Falsifiers

- `custctx refused: code word pin mismatch` (ERROR; pin / found / expected named) on the build the plugin accepted by build id: the code at that
  address is not what the wrapper was derived from (or the pin table has a typo); nothing is swapped, the game runs stock.
- `custctx dropped destroy ... why=not live: no context holds this address` in an ordinary lobby, repeatedly: a context is being
  created through some path other than LoadMeshData (or the Load slot is not reached): destroys are skipped and contexts/layouts leak
  (and `layouts_used` climbs on every re-entry).
- `cosmetic load refused` while `layouts_used` is well below capacity: the pool offsets (+0x84, +0x30) or the pool pointer are wrong.
- The same `memset(NULL)` crash in Grow with no `no memory` line: Grow was reached with data NULL by a path the count/capacity test
  does not see, or the slot is not what CTableXT callers use.
- Cosmetics missing for players on a roomy heap with `no memory for cosmetic draw entries` lines: `Resize` is not what Grow calls, or its
  argument convention differs.
- `custctx refused ... hook=Grow` etc: that slot does not hold the pinned function (nothing stays swapped).

## Needs a word patch / not built

- **Needs a word patch:** none.
- Not built: the md5 gate (deliberately none), `setprop` switch of the source (see above).
- Unverified: `SR15NetCustomDrawData` size 0x9718 beyond Grow's `mov w9,#0x9718`; the semantics of `CMemBlock::Resize` leaving `data`
  NULL on failure (source `:21-24`); that CreateContext checks LoadMeshData's non-zero result and returns NULL (source `:33-35`,
  "checked at 0x2113068", not re-read); the mesh-entry-per-layout assumption behind the margin 64. I read Grow's body (above), the
  RegisterRTLayout words, the PLT stubs and the function sizes, not LoadMeshData or DestroyContext bodies.
